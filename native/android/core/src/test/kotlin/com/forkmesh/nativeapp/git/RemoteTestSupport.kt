package com.forkmesh.nativeapp.git

class RecordedRequest(val request: GitHttpRequest) {
    val bodyText: String get() = latin1Text(request.body)
}

class FakeTransport(private val handler: (GitHttpRequest) -> GitHttpResponse) : GitHttpTransport {
    val requests = ArrayList<RecordedRequest>()
    var closed = false

    override fun send(
        request: GitHttpRequest,
        cancellation: GitCancellationToken?,
        limits: GitResourceLimits?,
    ): GitHttpResponse {
        cancellation?.throwIfCancelled()
        requests.add(RecordedRequest(request))
        val response = handler(request)
        if (response.body.size > request.maxResponseBytes) {
            throw GitException(GitErrorCode.resourceLimitExceeded, "response body exceeds the configured limit")
        }
        return response
    }

    override fun close() {
        closed = true
    }
}

fun packets(lines: Iterable<GitPktLine>): ByteArray {
    var output = ByteArray(0)
    for (line in lines) {
        output += line.encode()
    }
    return output
}

fun dataLine(text: String): GitPktLine = GitPktLine.data(utf8(text))

fun uploadPackAdvertisementBody(
    capabilities: List<String> = listOf(
        "agent=git/test",
        "ls-refs=unborn",
        "fetch=shallow wait-for-done",
        "object-format=sha1",
    ),
): ByteArray = packets(
    listOf(dataLine("# service=git-upload-pack\n"), GitPktLine.flush(), dataLine("version 2\n")) +
        capabilities.map { dataLine("$it\n") } +
        GitPktLine.flush(),
)

fun lsRefsBody(records: List<String>): ByteArray = packets(records.map { dataLine("$it\n") } + GitPktLine.flush())

fun acknowledgmentsOnlyBody(lines: List<String>): ByteArray =
    packets(listOf(dataLine("acknowledgments\n")) + lines.map { dataLine("$it\n") } + GitPktLine.flush())

fun packfileBody(packBytes: ByteArray, acknowledgments: List<String> = emptyList()): ByteArray {
    val chunks = ArrayList<GitPktLine>()
    var offset = 0
    while (offset < packBytes.size) {
        val end = if (offset + 8192 < packBytes.size) offset + 8192 else packBytes.size
        chunks.add(GitPktLine.data(byteArrayOf(1) + packBytes.copyOfRange(offset, end)))
        offset += 8192
    }
    val lines = ArrayList<GitPktLine>()
    if (acknowledgments.isNotEmpty()) {
        lines.add(dataLine("acknowledgments\n"))
        for (line in acknowledgments) lines.add(dataLine("$line\n"))
        lines.add(GitPktLine.delimiter())
    }
    lines.add(dataLine("packfile\n"))
    lines.addAll(chunks)
    lines.add(GitPktLine.flush())
    return packets(lines)
}

fun receivePackAdvertisementBody(
    refs: Map<String, GitObjectId>,
    capabilities: String = "report-status report-status-v2 delete-refs side-band-64k quiet " +
        "atomic ofs-delta object-format=sha1 agent=git/test",
): ByteArray {
    val lines = arrayListOf(dataLine("# service=git-receive-pack\n"), GitPktLine.flush())
    if (refs.isEmpty()) {
        lines.add(GitPktLine.data(utf8("${"0".repeat(40)} capabilities^{}") + byteArrayOf(0) + utf8("$capabilities\n")))
    } else {
        var first = true
        for ((name, id) in refs) {
            if (first) {
                lines.add(GitPktLine.data(utf8("${id.hex} $name") + byteArrayOf(0) + utf8("$capabilities\n")))
                first = false
            } else {
                lines.add(dataLine("${id.hex} $name\n"))
            }
        }
    }
    lines.add(GitPktLine.flush())
    return packets(lines)
}

fun sidebandReportBody(reportLines: List<String>): ByteArray {
    val report = packets(reportLines.map { dataLine("$it\n") } + GitPktLine.flush())
    return packets(listOf(GitPktLine.data(byteArrayOf(1) + report), GitPktLine.flush()))
}

fun okResponse(request: GitHttpRequest, body: ByteArray): GitHttpResponse =
    GitHttpResponse(statusCode = 200, headers = mapOf("content-type" to request.expectedContentType), body = body)
