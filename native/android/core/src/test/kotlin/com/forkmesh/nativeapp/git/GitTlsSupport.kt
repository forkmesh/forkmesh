package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertEq
import com.sun.net.httpserver.HttpsConfigurator
import com.sun.net.httpserver.HttpsServer
import java.io.File
import java.io.FileInputStream
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.URI
import java.security.KeyStore
import java.util.Base64
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import javax.net.ssl.KeyManagerFactory
import javax.net.ssl.SSLContext

class SelfSignedTls private constructor(val serverContext: SSLContext, val certificateDer: ByteArray) {
    val certificatePem: ByteArray
        get() = (
            "-----BEGIN CERTIFICATE-----\n" +
                Base64.getMimeEncoder(64, "\n".toByteArray()).encodeToString(certificateDer) +
                "\n-----END CERTIFICATE-----\n"
            ).toByteArray(Charsets.US_ASCII)

    companion object {
        private const val alias = "forkmesh-test"
        private val password = "forkmesh-test".toCharArray()

        fun keytool(): File? = File(File(System.getProperty("java.home"), "bin"), "keytool").takeIf { it.canExecute() }

        val shared: SelfSignedTls? by lazy { if (keytool() == null) null else create() }

        private fun create(): SelfSignedTls = withTempDir("forkmesh-tls-") { scratch ->
            val keystore = scratch.child("server.p12")
            val generated = runProcess(
                listOf(
                    keytool()!!.path,
                    "-genkeypair",
                    "-alias", alias,
                    "-keyalg", "RSA",
                    "-keysize", "2048",
                    "-dname", "CN=localhost",
                    "-ext", "SAN=ip:127.0.0.1,dns:localhost",
                    "-validity", "1",
                    "-keystore", keystore.path,
                    "-storetype", "PKCS12",
                    "-storepass", String(password),
                    "-keypass", String(password),
                ),
            )
            assertEq(0, generated.exitCode, generated.stderr)
            val keyStore = KeyStore.getInstance("PKCS12")
            FileInputStream(keystore).use { keyStore.load(it, password) }
            val keyManagers = KeyManagerFactory.getInstance(KeyManagerFactory.getDefaultAlgorithm())
            keyManagers.init(keyStore, password)
            val context = SSLContext.getInstance("TLS")
            context.init(keyManagers.keyManagers, null, null)
            SelfSignedTls(context, keyStore.getCertificate(alias).encoded)
        }
    }
}

class TestHttpsServer private constructor(val server: HttpsServer, private val executor: ExecutorService) : AutoCloseable {
    val port: Int get() = server.address.port

    fun url(path: String): URI = URI("https://127.0.0.1:$port$path")

    override fun close() {
        server.stop(0)
        executor.shutdownNow()
    }

    companion object {
        fun start(tls: SelfSignedTls, configure: (HttpsServer) -> Unit): TestHttpsServer {
            val server = HttpsServer.create(InetSocketAddress(InetAddress.getLoopbackAddress(), 0), 0)
            server.httpsConfigurator = HttpsConfigurator(tls.serverContext)
            val executor = Executors.newCachedThreadPool { runnable ->
                Thread(runnable, "git-test-https").also { it.isDaemon = true }
            }
            server.executor = executor
            configure(server)
            server.start()
            return TestHttpsServer(server, executor)
        }
    }
}
