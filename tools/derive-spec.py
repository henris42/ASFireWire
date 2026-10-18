#!/usr/bin/env python3
"""Derive an xcodegen spec for an older macOS release from project.yml.

    tools/derive-spec.py <macos> <driverkit> <full|lite> <arming 0|1> <out.yml>

project.yml targets the current release. The derived spec lowers the deployment
targets, optionally gates the dext on asfw-armed, and below macOS 14 turns the
app into the lite variant: the same app without the Metal audio analyzer,
meters, DV capture and the MCP server (macOS 13 package), compiled with
ASFW_LITE so Compat/MacOS12Compat.swift supplies the missing SwiftUI API, and
with the loader's headless --activate/--deactivate/--status for install.sh.
"""

import re
import sys

LITE_EXCLUDES = [
    "AudioObserver/**",
    "MCP/**",
    "**/*.metal",
    "Views/Analyzer*.swift",
    "Views/AudioAnalyzerView.swift",
    "Views/AudioObserverView.swift",
    "Views/AudioTelemetryView.swift",
    "Views/RxTelemetrySection.swift",
    "Views/StereoMetersView.swift",
    "Views/StereoHistoryView.swift",
    "Views/LoudnessHistoryView.swift",
    "Views/DVCaptureView.swift",
    "Views/MCPSettingsView.swift",
    "ViewModels/ASFWMCPControlViewModel.swift",
]


def main():
    macos, driverkit, app_kind, arming, out = sys.argv[1:6]
    s = open("project.yml").read()
    s, n_mac = re.subn(r'(MACOSX_DEPLOYMENT_TARGET: )"[0-9.]+"', rf'\g<1>"{macos}"', s)
    s, n_dk = re.subn(r'(DRIVERKIT_DEPLOYMENT_TARGET: )"[0-9.]+"', rf'\g<1>"{driverkit}"', s)
    if n_mac == 0 or n_dk == 0:
        sys.exit("project.yml: deployment target keys not found")
    if arming == "1":
        s, n_arm = re.subn(r'(\n( *)- "ASFW_MOTU_V1_VALIDATION=\$\(ASFW_MOTU_V1_VALIDATION\)"\n)',
                           r'\1\2- "ASFW_REQUIRE_ARMING=1"\n', s, count=1)
        if n_arm != 1:
            sys.exit("project.yml: driver preprocessor definitions not found")
    if app_kind == "lite":
        excludes = "".join(f'          - "{e}"\n' for e in LITE_EXCLUDES)
        s, n_src = re.subn(
            r'(\n  ASFW:\n    type: application\n(?:.*\n)*?    sources:\n)(?:      #.*\n)?      - path: ASFW\n',
            r'\1      - path: ASFW\n        excludes:\n' + excludes.replace("\\", "\\\\")
            + r'      - path: ASFWLoader/LoaderCommandLine.swift\n', s, count=1)
        if n_src != 1:
            sys.exit("project.yml: ASFW app target sources not found")
        # The MCP SDK requires macOS 13 and the lite app does not serve MCP.
        s, n_pkg = re.subn(r'\n *- package: swift-sdk\n *product: MCP(?=\n)', '', s)
        if n_pkg == 0:
            sys.exit("project.yml: MCP package dependency not found")
        s, n_cond = re.subn(r'(\n        PRODUCT_BUNDLE_IDENTIFIER: net\.asfw\.app\n)',
                            r'\1        SWIFT_ACTIVE_COMPILATION_CONDITIONS: ["$(inherited)", "ASFW_LITE"]\n',
                            s, count=1)
        if n_cond != 1:
            sys.exit("project.yml: app bundle identifier not found")
    open(out, "w").write(s)


if __name__ == "__main__":
    main()
