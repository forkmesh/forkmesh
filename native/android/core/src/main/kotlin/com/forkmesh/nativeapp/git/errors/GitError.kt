package com.forkmesh.nativeapp.git.errors

@Suppress("EnumEntryName")
enum class GitErrorCode {
    invalidRepository,
    unsupportedRepositoryFormat,
    invalidObject,
    objectHashMismatch,
    invalidTree,
    invalidRef,
    refLocked,
    indexCorrupt,
    packCorrupt,
    packChecksumMismatch,
    deltaDepthExceeded,
    resourceLimitExceeded,
    pathEscapesWorktree,
    symlinkEscape,
    networkUnavailable,
    tlsRejected,
    authenticationFailed,
    protocolUnsupported,
    remoteRejected,
    nonFastForward,
    cancelled,
    operationInterrupted,
    sha1CollisionDetected,
    hashScreeningUnavailable,
    hashImplementationFailure,
    quarantineRejected,
    ;

    val wireName: String get() = name
}

class GitException(val code: GitErrorCode, override val message: String) : RuntimeException(message) {
    override fun toString(): String = "GitException(code: ${code.wireName}, message: $message)"
}
