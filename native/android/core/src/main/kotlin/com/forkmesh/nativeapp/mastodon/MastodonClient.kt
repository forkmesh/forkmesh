package com.forkmesh.nativeapp.mastodon

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.security.SecretStore
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.util.UUID

class MastodonUnauthorized : Exception("Mastodon refused the sign-in")

class MastodonException(message: String) : Exception(message)

fun interface MastodonTransport {
    fun send(request: HttpClient.Request): HttpClient.Response

    companion object {
        val Default = MastodonTransport { HttpClient.send(it) }
    }
}

sealed class MastodonSource {
    data object Home : MastodonSource()
    data object Local : MastodonSource()
    data object Federated : MastodonSource()
    data object Bookmarks : MastodonSource()
    data object Favourites : MastodonSource()
    data class Tag(val name: String) : MastodonSource()
    data class Account(val id: String) : MastodonSource()

    val path: String
        get() = when (this) {
            Home -> "/api/v1/timelines/home?limit=${Mastodon.PAGE}"
            Local -> "/api/v1/timelines/public?local=true&limit=${Mastodon.PAGE}"
            Federated -> "/api/v1/timelines/public?limit=${Mastodon.PAGE}"
            Bookmarks -> "/api/v1/bookmarks?limit=${Mastodon.PAGE}"
            Favourites -> "/api/v1/favourites?limit=${Mastodon.PAGE}"
            is Tag -> "/api/v1/timelines/tag/${MastodonClient.segment(name)}?limit=${Mastodon.PAGE}"
            is Account -> "/api/v1/accounts/${MastodonClient.segment(id)}/statuses?limit=${Mastodon.PAGE}"
        }
}

data class MastodonUpload(val bytes: ByteArray, val fileName: String, val contentType: String, val description: String = "")

class MastodonClient(
    val server: String,
    val token: String = "",
    private val transport: MastodonTransport = MastodonTransport.Default,
    private val sleep: (Long) -> Unit = { Thread.sleep(it) },
) {
    fun registerApp(): MastodonApp {
        val json = form(
            "/api/v1/apps",
            listOf(
                "client_name" to Mastodon.CLIENT_NAME,
                "redirect_uris" to "${Mastodon.REDIRECT}\n${Mastodon.OOB}",
                "scopes" to Mastodon.SCOPES,
                "website" to Mastodon.WEBSITE,
            ),
        ).asJsonMap()
        val clientId = json.str("client_id").ifEmpty { throw MastodonException("This server could not register ForkMesh.") }
        return MastodonApp(clientId, json.str("client_secret"))
    }

    fun token(app: MastodonApp, code: String, redirect: String, verifier: String): String {
        val json = form(
            "/oauth/token",
            listOf(
                "grant_type" to "authorization_code",
                "code" to code,
                "client_id" to app.clientId,
                "client_secret" to app.clientSecret,
                "redirect_uri" to redirect,
                "code_verifier" to verifier,
                "scope" to Mastodon.SCOPES,
            ),
        ).asJsonMap()
        return json.str("access_token").ifEmpty { throw MastodonException("Mastodon did not return an access token.") }
    }

    fun revoke(app: MastodonApp) {
        runCatching {
            form("/oauth/revoke", listOf("client_id" to app.clientId, "client_secret" to app.clientSecret, "token" to token))
        }
    }

    fun me(): MastodonAccount = MastodonAccount.from(get("/api/v1/accounts/verify_credentials").asJsonMap())
        ?: throw MastodonException("Mastodon did not return your account.")

    fun instance(): MastodonInstanceLimits =
        runCatching { MastodonInstanceLimits.from(get("/api/v2/instance").asJsonMap()) }.getOrElse { failure ->
            if (failure is MastodonUnauthorized) throw failure
            MastodonInstanceLimits()
        }

    fun timeline(source: MastodonSource, next: String? = null): MastodonPage<MastodonStatus> {
        val response = call("GET", resolve(next) ?: url(source.path))
        return MastodonPage(MastodonStatus.list(parse(response)), MastodonPaging.next(response.header("Link"), server))
    }

    fun notifications(next: String? = null): MastodonPage<MastodonNotification> {
        val response = call("GET", resolve(next) ?: url("/api/v1/notifications?limit=${Mastodon.PAGE}"))
        return MastodonPage(jsonMaps(parse(response)).mapNotNull(MastodonNotification::from), MastodonPaging.next(response.header("Link"), server))
    }

    fun status(id: String): MastodonStatus = statusOf(get("/api/v1/statuses/${segment(id)}"))

    fun thread(id: String): MastodonThread {
        val status = status(id)
        val context = get("/api/v1/statuses/${segment(id)}/context").asJsonMap()
        return MastodonThread(MastodonStatus.list(context["ancestors"]), status, MastodonStatus.list(context["descendants"]))
    }

    fun favourite(id: String, on: Boolean) = action(id, if (on) "favourite" else "unfavourite")

    fun reblog(id: String, on: Boolean) = action(id, if (on) "reblog" else "unreblog")

    fun bookmark(id: String, on: Boolean) = action(id, if (on) "bookmark" else "unbookmark")

    private fun action(id: String, name: String): MastodonStatus =
        statusOf(parse(call("POST", url("/api/v1/statuses/${segment(id)}/$name")))).shown

    fun delete(id: String) {
        call("DELETE", url("/api/v1/statuses/${segment(id)}"))
    }

    fun post(
        text: String,
        visibility: String,
        spoiler: String = "",
        replyTo: String? = null,
        mediaIds: List<String> = emptyList(),
        idempotencyKey: String = UUID.randomUUID().toString(),
    ): MastodonStatus {
        val body = linkedMapOf<String, Any?>("status" to text, "visibility" to visibility)
        if (spoiler.isNotEmpty()) {
            body["spoiler_text"] = spoiler
            body["sensitive"] = true
        }
        if (!replyTo.isNullOrEmpty()) body["in_reply_to_id"] = replyTo
        if (mediaIds.isNotEmpty()) body["media_ids"] = mediaIds
        val response = call(
            "POST",
            url("/api/v1/statuses"),
            Json.stringify(body).toByteArray(Charsets.UTF_8),
            "application/json",
            mapOf("Idempotency-Key" to idempotencyKey),
        )
        return statusOf(parse(response))
    }

    fun upload(file: MastodonUpload): MastodonAttachment {
        val boundary = "ForkMesh" + UUID.randomUUID().toString().replace("-", "")
        val out = ByteArrayOutputStream()
        fun line(text: String) = out.write("$text\r\n".toByteArray(Charsets.UTF_8))
        line("--$boundary")
        line("Content-Disposition: form-data; name=\"file\"; filename=\"${file.fileName.replace("\"", "")}\"")
        line("Content-Type: ${file.contentType}")
        line("")
        out.write(file.bytes)
        line("")
        if (file.description.isNotBlank()) {
            line("--$boundary")
            line("Content-Disposition: form-data; name=\"description\"")
            line("")
            line(file.description.trim())
        }
        line("--$boundary--")
        val response = call("POST", url("/api/v2/media"), out.toByteArray(), "multipart/form-data; boundary=$boundary")
        var attachment = MastodonAttachment.from(parse(response).asJsonMap())
            ?: throw MastodonException("Mastodon did not accept the picture.")
        if (response.status != 202 && attachment.url.isNotEmpty()) return attachment
        repeat(10) {
            sleep(1000)
            val current = MastodonAttachment.from(get("/api/v1/media/${segment(attachment.id)}").asJsonMap())
            if (current != null) {
                attachment = current
                if (current.url.isNotEmpty()) return current
            }
        }
        return attachment
    }

    fun account(id: String): MastodonAccount = MastodonAccount.from(get("/api/v1/accounts/${segment(id)}").asJsonMap())
        ?: throw MastodonException("That account was not found.")

    fun relationship(id: String): MastodonRelationship? =
        jsonMaps(get("/api/v1/accounts/relationships?id%5B%5D=${MastodonForm.enc(id)}")).firstNotNullOfOrNull(MastodonRelationship::from)

    fun follow(id: String, on: Boolean): MastodonRelationship =
        MastodonRelationship.from(parse(call("POST", url("/api/v1/accounts/${segment(id)}/${if (on) "follow" else "unfollow"}"))).asJsonMap())
            ?: throw MastodonException("Mastodon did not answer.")

    fun search(query: String): MastodonSearch {
        val json = get("/api/v2/search?q=${MastodonForm.enc(query)}&resolve=true&limit=20").asJsonMap()
        return MastodonSearch(
            json.maps("accounts").mapNotNull(MastodonAccount::from),
            json.maps("hashtags").mapNotNull(MastodonTag::from),
            json.maps("statuses").mapNotNull(MastodonStatus::from),
        )
    }

    private fun statusOf(value: Any?): MastodonStatus = MastodonStatus.from(value.asJsonMap())
        ?: throw MastodonException("Mastodon did not return the post.")

    private fun get(path: String): Any? = parse(call("GET", url(path)))

    private fun form(path: String, fields: List<Pair<String, String>>): Any? = parse(
        call("POST", url(path), MastodonForm.encode(fields).toByteArray(Charsets.UTF_8), "application/x-www-form-urlencoded"),
    )

    private fun url(path: String) = server + path

    private fun resolve(next: String?): String? = next?.takeIf { it.startsWith("$server/") }

    private fun parse(response: HttpClient.Response): Any? =
        if (response.body.isEmpty()) null else runCatching { response.json() }.getOrNull()

    private fun call(
        method: String,
        url: String,
        body: ByteArray? = null,
        contentType: String? = null,
        headers: Map<String, String> = emptyMap(),
    ): HttpClient.Response {
        val request = HttpClient.Request(method, url).header("Accept", "application/json").bearer(token)
        headers.forEach { (name, value) -> request.header(name, value) }
        if (body != null) request.body(body, contentType)
        val response = try {
            transport.send(request)
        } catch (_: IOException) {
            throw MastodonException("Could not reach ${MastodonServer.host(server)}.")
        }
        if (response.status == 401 && token.isNotEmpty()) throw MastodonUnauthorized()
        if (!response.ok) {
            val message = runCatching { response.jsonMap().str("error") }.getOrDefault("")
            throw MastodonException(message.ifBlank { "Mastodon returned ${response.status}." })
        }
        return response
    }

    companion object {
        fun segment(value: String): String = MastodonForm.enc(value).replace("+", "%20")
    }
}

class MastodonStore(private val secrets: SecretStore) {
    var session: MastodonSession?
        get() = MastodonSession.parse(secrets.read(SESSION))
        set(value) = if (value == null) secrets.delete(SESSION) else secrets.write(SESSION, value.toJson())

    var pending: MastodonPending?
        get() = MastodonPending.parse(secrets.read(PENDING))
        set(value) = if (value == null) secrets.delete(PENDING) else secrets.write(PENDING, value.toJson())

    companion object {
        const val SESSION = "mastodon.session.v1"
        const val PENDING = "mastodon.pending.v1"
    }
}

class MastodonSignIn(
    private val store: MastodonStore,
    private val client: (server: String, token: String) -> MastodonClient = { server, token -> MastodonClient(server, token) },
) {
    fun start(input: String, redirect: String = Mastodon.REDIRECT): String {
        val server = MastodonServer.normalize(input)
            ?: throw MastodonException("Enter your server, such as mastodon.social.")
        val app = client(server, "").registerApp()
        val pkce = MastodonAuth.pkce()
        store.pending = MastodonPending(server, app.clientId, app.clientSecret, pkce.state, pkce.verifier, redirect)
        return MastodonAuth.authorizeUrl(server, app.clientId, redirect, pkce.state, pkce.challenge)
    }

    fun finish(input: String): MastodonSession {
        val pending = store.pending ?: throw MastodonException("Start the sign-in again.")
        val code = MastodonAuth.code(input, pending.state)
            ?: throw MastodonException("That code did not come from this sign-in. Start again.")
        val app = MastodonApp(pending.clientId, pending.clientSecret)
        val token = client(pending.server, "").token(app, code, pending.redirect, pending.verifier)
        val account = client(pending.server, token).me()
        val session = MastodonSession(pending.server, token, app.clientId, app.clientSecret).withAccount(account)
        store.session = session
        store.pending = null
        return session
    }

    fun withToken(input: String, token: String): MastodonSession {
        val server = MastodonServer.normalize(input)
            ?: throw MastodonException("Enter your server, such as mastodon.social.")
        val trimmed = token.trim().ifEmpty { throw MastodonException("Paste the access token first.") }
        val account = try {
            client(server, trimmed).me()
        } catch (_: MastodonUnauthorized) {
            throw MastodonException("The server did not accept that token.")
        }
        val session = MastodonSession(server, trimmed).withAccount(account)
        store.session = session
        store.pending = null
        return session
    }

    fun forgetLocally(): MastodonSession? {
        val session = store.session
        store.session = null
        store.pending = null
        return session
    }

    fun revoke(session: MastodonSession) {
        client(session.server, session.token).revoke(MastodonApp(session.clientId, session.clientSecret))
    }

    fun signOut() {
        forgetLocally()?.let(::revoke)
    }
}
