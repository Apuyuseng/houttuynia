// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "MWineUI",
    platforms: [.macOS(.v14)],
    dependencies: [],
    targets: [
        .executableTarget(
            name: "MWineUI",
            path: "Sources"
        )
    ]
)
