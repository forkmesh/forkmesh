import SwiftUI
import UIKit

extension View {
    func shellKeepsNavigationBar() -> some View {
        background { ShellPageBarKeeper() }
    }
}

private struct ShellPageBarKeeper: UIViewRepresentable {
    func makeUIView(context: Context) -> ShellPageBarKeeperView {
        ShellPageBarKeeperView()
    }

    func updateUIView(_ uiView: ShellPageBarKeeperView, context: Context) {
        uiView.settle()
    }
}

private final class ShellPageBarKeeperView: UIView {
    override func didMoveToWindow() {
        super.didMoveToWindow()
        isUserInteractionEnabled = false
        settle()
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        settle()
    }

    override func safeAreaInsetsDidChange() {
        super.safeAreaInsetsDidChange()
        settle()
    }

    func settle() {
        guard window != nil, let navigation = owningNavigation() else { return }
        if let coordinator = navigation.transitionCoordinator {
            coordinator.animate(alongsideTransition: nil) { [weak self] (_: UIViewControllerTransitionCoordinatorContext) in
                self?.showWhenOnTop()
            }
        } else {
            showWhenOnTop()
        }
    }

    private func showWhenOnTop() {
        guard window != nil, let navigation = owningNavigation(), navigation.isNavigationBarHidden,
              let top = navigation.topViewController?.viewIfLoaded, isDescendant(of: top) else { return }
        navigation.setNavigationBarHidden(false, animated: false)
    }

    private func owningNavigation() -> UINavigationController? {
        var responder: UIResponder? = next
        while let current = responder {
            if let navigation = current as? UINavigationController {
                return navigation
            }
            responder = current.next
        }
        return nil
    }
}
