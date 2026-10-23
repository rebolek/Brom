Recoil-Package [
    Name: recoil-gui
    Version: 0.1.0
    Module-Roots: [gui]
    Install: [
        Capability: gui
        Display-Name: "GUI window + 2D drawing + input"
        Requires: [gui-backend]
    ]
    Vendor: [
        Export: [
            Includes: [%include/]
        ]
    ]
]
