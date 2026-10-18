import SwiftUI

struct StoreView: View {
    let store: MemoryStore

    var body: some View {
        List(store.load(), id: \.self) { value in
            if value.count > 3 {
                Text(value)
            } else {
                Text("short")
            }
        }
    }
}
