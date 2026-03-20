import Foundation

class ViewController {
    private var titleLabel: String = ""
    private var items: [String] = []
    private var isLoaded: Bool = false

    func viewDidLoad() {
        titleLabel = "Main View"
        items = ["Dashboard", "Settings", "Profile"]
        isLoaded = true

        let timestamp = Date()
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd HH:mm:ss"
        print("View loaded at \(formatter.string(from: timestamp))")

        updateUI(title: titleLabel, itemCount: items.count)
    }

    func handleTap(at index: Int) {
        guard isLoaded else {
            print("Error: view not loaded")
            return
        }

        guard index >= 0 && index < items.count else {
            print("Error: index \(index) out of range (0..<\(items.count))")
            return
        }

        let selected = items[index]
        print("Selected: \(selected)")
        updateUI(title: selected, itemCount: items.count)
    }

    func updateUI(title: String, itemCount: Int) {
        titleLabel = title
        let status = itemCount > 0 ? "(\(itemCount) items)" : "(empty)"
        print("UI updated: \(titleLabel) \(status)")
    }
}

func createScene(name: String? = nil) -> ViewController {
    let controller = ViewController()
    controller.viewDidLoad()
    if let sceneName = name {
        print("Scene created: \(sceneName)")
    }
    return controller
}
