package com.forkmesh.nativeapp.git

object ForkMeshGit {
    const val agent: String = "forkmesh-git/0.0.0"
}

typealias GitErrorCode = com.forkmesh.nativeapp.git.errors.GitErrorCode
typealias GitException = com.forkmesh.nativeapp.git.errors.GitException

typealias GitResourceLimits = com.forkmesh.nativeapp.git.security.GitResourceLimits
typealias GitOperationJournal = com.forkmesh.nativeapp.git.security.GitOperationJournal
typealias GitCancellationToken = com.forkmesh.nativeapp.git.security.GitCancellationToken
typealias GitObjectQuarantine = com.forkmesh.nativeapp.git.security.GitObjectQuarantine

typealias CheckedBytesReader = com.forkmesh.nativeapp.git.bytes.CheckedBytesReader
typealias CheckedBytesWriter = com.forkmesh.nativeapp.git.bytes.CheckedBytesWriter

typealias GitSha1ObjectOrigin = com.forkmesh.nativeapp.git.crypto.GitSha1ObjectOrigin
typealias GitSha1CollisionDetectorStatus = com.forkmesh.nativeapp.git.crypto.GitSha1CollisionDetectorStatus
typealias GitSha1CollisionDetector = com.forkmesh.nativeapp.git.crypto.GitSha1CollisionDetector
typealias GitSha1CollisionDetectorSession = com.forkmesh.nativeapp.git.crypto.GitSha1CollisionDetectorSession
typealias GitSha1ScreeningDecision = com.forkmesh.nativeapp.git.crypto.GitSha1ScreeningDecision
typealias GitSha1ScreeningResult = com.forkmesh.nativeapp.git.crypto.GitSha1ScreeningResult
typealias Sha1CollisionScreen = com.forkmesh.nativeapp.git.crypto.Sha1CollisionScreen
typealias GitSha1ScreeningSession = com.forkmesh.nativeapp.git.crypto.GitSha1ScreeningSession
typealias GitDigest = com.forkmesh.nativeapp.git.crypto.GitDigest
typealias ForkMeshSha1 = com.forkmesh.nativeapp.git.crypto.ForkMeshSha1
typealias ForkMeshSha256 = com.forkmesh.nativeapp.git.crypto.ForkMeshSha256

typealias GitIndexStore = com.forkmesh.nativeapp.git.index.GitIndexStore
typealias GitIndexMode = com.forkmesh.nativeapp.git.index.GitIndexMode
typealias GitIndexEntry = com.forkmesh.nativeapp.git.index.GitIndexEntry
typealias GitIndexV2 = com.forkmesh.nativeapp.git.index.GitIndexV2

typealias GitObjectType = com.forkmesh.nativeapp.git.objects.GitObjectType
typealias GitObjectId = com.forkmesh.nativeapp.git.objects.GitObjectId
typealias GitObjectFrame = com.forkmesh.nativeapp.git.objects.GitObjectFrame
typealias LooseObjectStore = com.forkmesh.nativeapp.git.objects.LooseObjectStore
typealias GitCommitIdentity = com.forkmesh.nativeapp.git.objects.GitCommitIdentity
typealias GitCommit = com.forkmesh.nativeapp.git.objects.GitCommit
typealias GitAnnotatedTag = com.forkmesh.nativeapp.git.objects.GitAnnotatedTag
typealias GitTreeMode = com.forkmesh.nativeapp.git.objects.GitTreeMode
typealias GitTreeEntry = com.forkmesh.nativeapp.git.objects.GitTreeEntry
typealias GitTree = com.forkmesh.nativeapp.git.objects.GitTree

typealias GitPackObject = com.forkmesh.nativeapp.git.pack.GitPackObject
typealias GitPack = com.forkmesh.nativeapp.git.pack.GitPack
typealias GitPackReader = com.forkmesh.nativeapp.git.pack.GitPackReader
typealias GitPackIndexEntry = com.forkmesh.nativeapp.git.pack.GitPackIndexEntry
typealias GitPackIndexV2 = com.forkmesh.nativeapp.git.pack.GitPackIndexV2
typealias GitPackObjectStore = com.forkmesh.nativeapp.git.pack.GitPackObjectStore
typealias GitPackWriteObject = com.forkmesh.nativeapp.git.pack.GitPackWriteObject
typealias GitWrittenPack = com.forkmesh.nativeapp.git.pack.GitWrittenPack
typealias GitPackWriter = com.forkmesh.nativeapp.git.pack.GitPackWriter
typealias GitPackIndexWriteEntry = com.forkmesh.nativeapp.git.pack.GitPackIndexWriteEntry
typealias GitPackIndexWriter = com.forkmesh.nativeapp.git.pack.GitPackIndexWriter

typealias GitPktLineKind = com.forkmesh.nativeapp.git.protocol.GitPktLineKind
typealias GitPktLine = com.forkmesh.nativeapp.git.protocol.GitPktLine
typealias GitSidebandResult = com.forkmesh.nativeapp.git.protocol.GitSidebandResult
typealias GitSideband = com.forkmesh.nativeapp.git.protocol.GitSideband
typealias GitProtocolV2Advertisement = com.forkmesh.nativeapp.git.protocol.GitProtocolV2Advertisement
typealias GitRemoteRef = com.forkmesh.nativeapp.git.protocol.GitRemoteRef
typealias GitProtocolV2 = com.forkmesh.nativeapp.git.protocol.GitProtocolV2
typealias GitFetchResponse = com.forkmesh.nativeapp.git.protocol.GitFetchResponse
typealias GitPushRefUpdate = com.forkmesh.nativeapp.git.protocol.GitPushRefUpdate
typealias GitPushRefStatus = com.forkmesh.nativeapp.git.protocol.GitPushRefStatus
typealias GitReceivePackStatus = com.forkmesh.nativeapp.git.protocol.GitReceivePackStatus
typealias GitReceivePackAdvertisement = com.forkmesh.nativeapp.git.protocol.GitReceivePackAdvertisement
typealias GitReceivePack = com.forkmesh.nativeapp.git.protocol.GitReceivePack
typealias GitSmartHttpService = com.forkmesh.nativeapp.git.protocol.GitSmartHttpService
typealias GitSmartHttp = com.forkmesh.nativeapp.git.protocol.GitSmartHttp

typealias GitHttpHeaderProvider = com.forkmesh.nativeapp.git.transport.GitHttpHeaderProvider
typealias GitHttpHeaders = com.forkmesh.nativeapp.git.transport.GitHttpHeaders
typealias GitCertificateChallenge = com.forkmesh.nativeapp.git.transport.GitCertificateChallenge
typealias GitCertificateDecision = com.forkmesh.nativeapp.git.transport.GitCertificateDecision
typealias GitCertificateChallengeHandler = com.forkmesh.nativeapp.git.transport.GitCertificateChallengeHandler
typealias GitHttpRequest = com.forkmesh.nativeapp.git.transport.GitHttpRequest
typealias GitHttpResponse = com.forkmesh.nativeapp.git.transport.GitHttpResponse
typealias GitHttpTransport = com.forkmesh.nativeapp.git.transport.GitHttpTransport
typealias GitIoHttpTransport = com.forkmesh.nativeapp.git.transport.GitIoHttpTransport
typealias GitRemoteUrl = com.forkmesh.nativeapp.git.transport.GitRemoteUrl

typealias GitRefName = com.forkmesh.nativeapp.git.refs.GitRefName
typealias GitSymbolicRef = com.forkmesh.nativeapp.git.refs.GitSymbolicRef
typealias GitRefUpdateResult = com.forkmesh.nativeapp.git.refs.GitRefUpdateResult
typealias RefStore = com.forkmesh.nativeapp.git.refs.RefStore
typealias PackedRef = com.forkmesh.nativeapp.git.refs.PackedRef
typealias PackedRefs = com.forkmesh.nativeapp.git.refs.PackedRefs
typealias GitFetchRefspec = com.forkmesh.nativeapp.git.refs.GitFetchRefspec
typealias GitRemoteName = com.forkmesh.nativeapp.git.refs.GitRemoteName
typealias GitPushRefspec = com.forkmesh.nativeapp.git.refs.GitPushRefspec

typealias GitProgressPhase = com.forkmesh.nativeapp.git.repository.GitProgressPhase
typealias GitProgress = com.forkmesh.nativeapp.git.repository.GitProgress
typealias GitProgressListener = com.forkmesh.nativeapp.git.repository.GitProgressListener
typealias GitResult<T> = com.forkmesh.nativeapp.git.repository.GitResult<T>
typealias GitSuccess<T> = com.forkmesh.nativeapp.git.repository.GitSuccess<T>
typealias GitFailure<T> = com.forkmesh.nativeapp.git.repository.GitFailure<T>
typealias GitOperation<T> = com.forkmesh.nativeapp.git.repository.GitOperation<T>
typealias GitProgressSink = com.forkmesh.nativeapp.git.repository.GitProgressSink
typealias GitRepositoryInit = com.forkmesh.nativeapp.git.repository.GitRepositoryInit
typealias GitRepositoryOpen = com.forkmesh.nativeapp.git.repository.GitRepositoryOpen
typealias GitChangeKind = com.forkmesh.nativeapp.git.repository.GitChangeKind
typealias GitStatusEntry = com.forkmesh.nativeapp.git.repository.GitStatusEntry
typealias GitStatus = com.forkmesh.nativeapp.git.repository.GitStatus
typealias GitIdentity = com.forkmesh.nativeapp.git.repository.GitIdentity
typealias GitCommitRequest = com.forkmesh.nativeapp.git.repository.GitCommitRequest
typealias GitBranchName = com.forkmesh.nativeapp.git.repository.GitBranchName
typealias GitCheckoutRequest = com.forkmesh.nativeapp.git.repository.GitCheckoutRequest
typealias GitLogRequest = com.forkmesh.nativeapp.git.repository.GitLogRequest
typealias GitLogEntry = com.forkmesh.nativeapp.git.repository.GitLogEntry
typealias GitRefEntry = com.forkmesh.nativeapp.git.repository.GitRefEntry
typealias GitRepository = com.forkmesh.nativeapp.git.repository.GitRepository
typealias GitRemoteAccess = com.forkmesh.nativeapp.git.repository.GitRemoteAccess
typealias GitCloneRequest = com.forkmesh.nativeapp.git.repository.GitCloneRequest
typealias GitCloneResult = com.forkmesh.nativeapp.git.repository.GitCloneResult
typealias GitFetchRequest = com.forkmesh.nativeapp.git.repository.GitFetchRequest
typealias GitDeepen = com.forkmesh.nativeapp.git.repository.GitDeepen
typealias GitAncestry = com.forkmesh.nativeapp.git.repository.GitAncestry
typealias GitRefUpdateRecord = com.forkmesh.nativeapp.git.repository.GitRefUpdateRecord
typealias GitFetchResult = com.forkmesh.nativeapp.git.repository.GitFetchResult
typealias GitPushRequest = com.forkmesh.nativeapp.git.repository.GitPushRequest
typealias GitPushRefOutcome = com.forkmesh.nativeapp.git.repository.GitPushRefOutcome
typealias GitPushRefResult = com.forkmesh.nativeapp.git.repository.GitPushRefResult
typealias GitPushResult = com.forkmesh.nativeapp.git.repository.GitPushResult
typealias GitRemoteConfig = com.forkmesh.nativeapp.git.repository.GitRemoteConfig
typealias GitBranchConfig = com.forkmesh.nativeapp.git.repository.GitBranchConfig
typealias RepositoryConfig = com.forkmesh.nativeapp.git.repository.RepositoryConfig

typealias GitWorktreePath = com.forkmesh.nativeapp.git.worktree.GitWorktreePath
typealias GitWorktree = com.forkmesh.nativeapp.git.worktree.GitWorktree
typealias FileSystemEntityType = com.forkmesh.nativeapp.git.worktree.FileSystemEntityType
