// swift-tools-version:5.3
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let cSettings:[CSetting] = [
    .headerSearchPath("include"),
    .define("IN3_EXPORT_TEST=static"),
    .define("IN3_VERSION=\"3.1.0\""),
    .define("BTC_PRE_BPI34"),
    .define("PK_SIGNER"),
    .define("ZKSYNC"),
    .define("SWIFT"),
    .define("ETH_FULL"),
    .define("ETH_BASIC"),
    .define("ETH_NANO"),
    .define("ETH_API"),
    .define("IPFS"),
    .define("NODESELECT_DEF"),
    .define("THREADSAFE"),
    .define("SCRYPT"),
    .define("LOGGING"),
    .define("IN3_AUTOINIT_PATH=\"../../../../Sources/autoregister.h\""),
    .define("LTM_ALL"),    
]

let package = Package(
    name: "In3",
    platforms: [.iOS(.v14), .macOS(.v11)],
    products: [
        .library(
            name: "In3",
            targets: ["In3"]),
    ],
    dependencies: [],
    targets: [
        .target(
            name: "CIn3",
            path: "in3/c",
            exclude : [ 
                   "src/third-party/crypto/ed25519-donna",
                   "src/third-party/crypto/secp256k1.table"
            ],
            cSettings: cSettings  
        ),
        .target(
            name: "In3",
            dependencies: ["CIn3"]
        ),
    ]
)

