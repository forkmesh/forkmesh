// On-device models for the native iOS app: Gemma 4 through MLX, run by the
// app target; see native/ios/docs/FMLocalAI.md.
//
// This module holds everything that is not MLX: the model catalog, device
// fit, the Hugging Face snapshot download, the engine protocol the app
// implements, the view models, and the request handler that lets a paired
// peer use the phone's model. It builds and tests on Linux.

/// Namespace marker for the FMLocalAI module.
public enum FMLocalAIModule {
    public static let name = "FMLocalAI"
}
