import AppKit
import ApplicationServices
import CoreGraphics
import Darwin
import Foundation

// Compile with: xcrun swiftc -parse-as-library -O -framework AppKit \
//   -framework ApplicationServices -framework CoreGraphics q1_10_signed_ax_driver.swift -o q1_10_signed_ax_driver

private let expectedBundleIdentifier = "com.wincommander.App.CodexDev"
private let targetRowIdentifier = "wincommander.explorer.list.item"

private struct Options {
    let appURL: URL
    let fixtureURL: URL
    let targetPrefix: String
    let outputURL: URL
    let timeoutMilliseconds: Int
    let pollMilliseconds: Int
}

private struct MeasurementRecord: Codable {
    let schemaVersion: Int
    let status: String
    let timestampUTC: String
    let bundleIdentifier: String
    let appPath: String
    let fixturePath: String
    let targetPrefix: String
    let pid: Int32?
    let navigationInputUptimeNanoseconds: UInt64?
    let firstRowUptimeNanoseconds: UInt64?
    let firstRowLatencyMilliseconds: Double?
    let firstRowLabel: String?
    let selectionInputUptimeNanoseconds: UInt64?
    let selectionObservedUptimeNanoseconds: UInt64?
    let selectionLatencyMilliseconds: Double?
    let selectionLabel: String?
    let selectionInputKind: String?
    let firstRowPollCount: Int?
    let selectionPollCount: Int?
    let axNodesExamined: Int?
    let failurePhase: String?
    let error: String?

    enum CodingKeys: String, CodingKey {
        case schemaVersion = "schema_version"
        case status
        case timestampUTC = "timestamp_utc"
        case bundleIdentifier = "bundle_identifier"
        case appPath = "app_path"
        case fixturePath = "fixture_path"
        case targetPrefix = "target_prefix"
        case pid
        case navigationInputUptimeNanoseconds = "navigation_input_uptime_nanoseconds"
        case firstRowUptimeNanoseconds = "first_row_uptime_nanoseconds"
        case firstRowLatencyMilliseconds = "first_row_latency_ms"
        case firstRowLabel = "first_row_label"
        case selectionInputUptimeNanoseconds = "selection_input_uptime_nanoseconds"
        case selectionObservedUptimeNanoseconds = "selection_observed_uptime_nanoseconds"
        case selectionLatencyMilliseconds = "selection_latency_ms"
        case selectionLabel = "selection_label"
        case selectionInputKind = "selection_input_kind"
        case firstRowPollCount = "first_row_poll_count"
        case selectionPollCount = "selection_poll_count"
        case axNodesExamined = "ax_nodes_examined"
        case failurePhase = "failure_phase"
        case error
    }
}

private struct DriverFailure: Error {
    let phase: String
    let message: String
}

private struct RowObservation {
    let element: AXUIElement
    let label: String
    let focused: Bool
    let selected: Bool
}

private struct RowScan {
    let rows: [RowObservation]
    let nodesExamined: Int
    let truncated: Bool
}

private struct TimedRowObservation {
    let row: RowObservation
    let observedAt: UInt64
    let pollCount: Int
    let nodesExamined: Int
}

private func usage() -> String {
    """
    Usage:
      q1_10_signed_ax_driver --app /path/to/WinCommander-Codex.app
        --fixture /path/to/large-folder --target-prefix q1-100k-
        --output /path/to/results.jsonl [--timeout-ms 15000] [--poll-ms 20]

    The driver requires Accessibility permission. The fixture must be a real directory,
    and its visible file labels must begin with the supplied unique target prefix.
    """
}

private func parsePositiveInteger(_ value: String, name: String, maximum: Int) throws -> Int {
    guard let parsed = Int(value), parsed > 0, parsed <= maximum else {
        throw DriverFailure(phase: "arguments", message: "\(name) must be in 1...\(maximum)")
    }
    return parsed
}

private func parseOptions(_ arguments: [String]) throws -> Options {
    var values: [String: String] = [:]
    var index = 1
    while index < arguments.count {
        let key = arguments[index]
        guard key.hasPrefix("--"), index + 1 < arguments.count else {
            throw DriverFailure(phase: "arguments", message: "Invalid argument near '\(key)'\n\(usage())")
        }
        guard values[key] == nil else {
            throw DriverFailure(phase: "arguments", message: "Duplicate argument '\(key)'")
        }
        values[key] = arguments[index + 1]
        index += 2
    }

    let allowed = Set(["--app", "--fixture", "--target-prefix", "--output", "--timeout-ms", "--poll-ms"])
    if let unknown = values.keys.first(where: { !allowed.contains($0) }) {
        throw DriverFailure(phase: "arguments", message: "Unknown argument '\(unknown)'\n\(usage())")
    }

    guard let appPath = values["--app"],
          let fixturePath = values["--fixture"],
          let targetPrefix = values["--target-prefix"],
          let outputPath = values["--output"] else {
        throw DriverFailure(phase: "arguments", message: "Missing required argument\n\(usage())")
    }
    guard !targetPrefix.isEmpty else {
        throw DriverFailure(phase: "arguments", message: "--target-prefix must not be empty")
    }

    let timeout = try parsePositiveInteger(values["--timeout-ms"] ?? "15000",
                                           name: "--timeout-ms",
                                           maximum: 300_000)
    let poll = try parsePositiveInteger(values["--poll-ms"] ?? "20", name: "--poll-ms", maximum: 1_000)
    guard poll <= timeout else {
        throw DriverFailure(phase: "arguments", message: "--poll-ms must not exceed --timeout-ms")
    }

    let appURL = URL(fileURLWithPath: appPath).standardizedFileURL.resolvingSymlinksInPath()
    let fixtureURL = URL(fileURLWithPath: fixturePath).standardizedFileURL.resolvingSymlinksInPath()
    let outputURL = URL(fileURLWithPath: outputPath).standardizedFileURL
    let fileManager = FileManager.default

    var appIsDirectory: ObjCBool = false
    guard fileManager.fileExists(atPath: appURL.path, isDirectory: &appIsDirectory), appIsDirectory.boolValue else {
        throw DriverFailure(phase: "arguments", message: "Application does not exist: \(appURL.path)")
    }
    guard appURL.pathExtension == "app" else {
        throw DriverFailure(phase: "arguments", message: "--app must name an .app bundle")
    }

    var fixtureIsDirectory: ObjCBool = false
    guard fileManager.fileExists(atPath: fixtureURL.path, isDirectory: &fixtureIsDirectory),
          fixtureIsDirectory.boolValue else {
        throw DriverFailure(phase: "arguments", message: "Fixture directory does not exist: \(fixtureURL.path)")
    }

    let outputParent = outputURL.deletingLastPathComponent()
    var outputParentIsDirectory: ObjCBool = false
    guard fileManager.fileExists(atPath: outputParent.path, isDirectory: &outputParentIsDirectory),
          outputParentIsDirectory.boolValue else {
        throw DriverFailure(phase: "arguments", message: "Output parent directory does not exist: \(outputParent.path)")
    }

    return Options(appURL: appURL,
                   fixtureURL: fixtureURL,
                   targetPrefix: targetPrefix,
                   outputURL: outputURL,
                   timeoutMilliseconds: timeout,
                   pollMilliseconds: poll)
}

private func iso8601Now() -> String {
    let formatter = ISO8601DateFormatter()
    formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
    return formatter.string(from: Date())
}

private func monotonicNow() -> UInt64 {
    DispatchTime.now().uptimeNanoseconds
}

private func milliseconds(from start: UInt64, to end: UInt64) -> Double {
    Double(end - start) / 1_000_000.0
}

private func deadline(afterMilliseconds milliseconds: Int) -> UInt64 {
    monotonicNow() + UInt64(milliseconds) * 1_000_000
}

private func sleep(milliseconds: Int) {
    usleep(useconds_t(milliseconds * 1_000))
}

private func normalizedPath(_ url: URL) -> String {
    url.standardizedFileURL.resolvingSymlinksInPath().path
}

private func copyAttribute(_ element: AXUIElement, _ attribute: CFString) -> CFTypeRef? {
    var value: CFTypeRef?
    guard AXUIElementCopyAttributeValue(element, attribute, &value) == .success else {
        return nil
    }
    return value
}

private func stringAttribute(_ element: AXUIElement, _ attribute: CFString) -> String? {
    copyAttribute(element, attribute) as? String
}

private func booleanAttribute(_ element: AXUIElement, _ attribute: CFString) -> Bool {
    (copyAttribute(element, attribute) as? NSNumber)?.boolValue ?? false
}

private func elementAttribute(_ element: AXUIElement, _ attribute: CFString) -> AXUIElement? {
    guard let value = copyAttribute(element, attribute), CFGetTypeID(value) == AXUIElementGetTypeID() else {
        return nil
    }
    return (value as! AXUIElement)
}

private func elementArrayAttribute(_ element: AXUIElement, _ attribute: CFString) -> [AXUIElement]? {
    guard let value = copyAttribute(element, attribute), CFGetTypeID(value) == CFArrayGetTypeID() else {
        return nil
    }
    return value as? [AXUIElement]
}

private func pointAttribute(_ element: AXUIElement, _ attribute: CFString) -> CGPoint? {
    guard let raw = copyAttribute(element, attribute), CFGetTypeID(raw) == AXValueGetTypeID() else {
        return nil
    }
    let value = raw as! AXValue
    guard AXValueGetType(value) == .cgPoint else {
        return nil
    }
    var point = CGPoint.zero
    guard AXValueGetValue(value, .cgPoint, &point) else {
        return nil
    }
    return point
}

private func sizeAttribute(_ element: AXUIElement, _ attribute: CFString) -> CGSize? {
    guard let raw = copyAttribute(element, attribute), CFGetTypeID(raw) == AXValueGetTypeID() else {
        return nil
    }
    let value = raw as! AXValue
    guard AXValueGetType(value) == .cgSize else {
        return nil
    }
    var size = CGSize.zero
    guard AXValueGetValue(value, .cgSize, &size) else {
        return nil
    }
    return size
}

private func accessibilityLabel(_ element: AXUIElement) -> String? {
    for attribute in [kAXDescriptionAttribute as CFString, kAXTitleAttribute as CFString, kAXValueAttribute as CFString] {
        if let value = stringAttribute(element, attribute), !value.isEmpty {
            return value
        }
    }
    return nil
}

private func scanRows(in application: AXUIElement,
                      targetPrefix: String,
                      maximumNodes: Int = 4_096) -> RowScan {
    var queue: [AXUIElement] = [application]
    if let windows = elementArrayAttribute(application, kAXWindowsAttribute as CFString) {
        queue.append(contentsOf: windows)
    }
    var visited = Set<CFHashCode>()
    var rows: [RowObservation] = []
    var cursor = 0
    var examined = 0

    while cursor < queue.count && examined < maximumNodes {
        let element = queue[cursor]
        cursor += 1
        let identity = CFHash(element)
        if !visited.insert(identity).inserted {
            continue
        }
        examined += 1

        if stringAttribute(element, kAXIdentifierAttribute as CFString) == targetRowIdentifier {
            if let label = accessibilityLabel(element), label.hasPrefix(targetPrefix) {
                rows.append(RowObservation(element: element,
                                           label: label,
                                           focused: booleanAttribute(element, kAXFocusedAttribute as CFString),
                                           selected: booleanAttribute(element, kAXSelectedAttribute as CFString)))
            }
            continue
        }

        let role = stringAttribute(element, kAXRoleAttribute as CFString)
        if role == (kAXTableRole as String) || role == (kAXOutlineRole as String) {
            if let visibleRows = elementArrayAttribute(element, kAXVisibleRowsAttribute as CFString) {
                queue.append(contentsOf: visibleRows)
            }
            continue
        }

        if let children = elementArrayAttribute(element, kAXChildrenAttribute as CFString) {
            queue.append(contentsOf: children)
        }
    }

    return RowScan(rows: rows, nodesExamined: examined, truncated: cursor < queue.count)
}

private func focusedTextInput(in application: AXUIElement) -> AXUIElement? {
    guard let focused = elementAttribute(application, kAXFocusedUIElementAttribute as CFString),
          let role = stringAttribute(focused, kAXRoleAttribute as CFString) else {
        return nil
    }
    let acceptedRoles = Set([kAXTextFieldRole as String, kAXTextAreaRole as String, kAXComboBoxRole as String])
    return acceptedRoles.contains(role) ? focused : nil
}

private func postKey(_ keyCode: CGKeyCode, flags: CGEventFlags = []) throws {
    let source = CGEventSource(stateID: .hidSystemState)
    guard let down = CGEvent(keyboardEventSource: source, virtualKey: keyCode, keyDown: true),
          let up = CGEvent(keyboardEventSource: source, virtualKey: keyCode, keyDown: false) else {
        throw DriverFailure(phase: "input", message: "Unable to create keyboard CGEvent")
    }
    down.flags = flags
    up.flags = flags
    down.post(tap: .cghidEventTap)
    usleep(5_000)
    up.post(tap: .cghidEventTap)
}

private func postUnicodeText(_ text: String) throws {
    let source = CGEventSource(stateID: .hidSystemState)
    guard let down = CGEvent(keyboardEventSource: source, virtualKey: 0, keyDown: true),
          let up = CGEvent(keyboardEventSource: source, virtualKey: 0, keyDown: false) else {
        throw DriverFailure(phase: "input", message: "Unable to create Unicode CGEvent")
    }
    var units = Array(text.utf16)
    down.keyboardSetUnicodeString(stringLength: units.count, unicodeString: &units)
    up.keyboardSetUnicodeString(stringLength: units.count, unicodeString: &units)
    down.post(tap: .cghidEventTap)
    usleep(5_000)
    up.post(tap: .cghidEventTap)
}

private func postCommandClick(at point: CGPoint) throws {
    let source = CGEventSource(stateID: .hidSystemState)
    guard let moved = CGEvent(mouseEventSource: source,
                              mouseType: .mouseMoved,
                              mouseCursorPosition: point,
                              mouseButton: .left),
          let down = CGEvent(mouseEventSource: source,
                             mouseType: .leftMouseDown,
                             mouseCursorPosition: point,
                             mouseButton: .left),
          let up = CGEvent(mouseEventSource: source,
                           mouseType: .leftMouseUp,
                           mouseCursorPosition: point,
                           mouseButton: .left) else {
        throw DriverFailure(phase: "selection_input", message: "Unable to create mouse CGEvent")
    }
    down.flags = .maskCommand
    up.flags = .maskCommand
    moved.post(tap: .cghidEventTap)
    usleep(5_000)
    down.post(tap: .cghidEventTap)
    usleep(5_000)
    up.post(tap: .cghidEventTap)
}

private func waitForWindow(in application: AXUIElement, timeoutMilliseconds: Int, pollMilliseconds: Int) throws {
    let limit = deadline(afterMilliseconds: timeoutMilliseconds)
    while monotonicNow() < limit {
        if let windows = elementArrayAttribute(application, kAXWindowsAttribute as CFString), !windows.isEmpty {
            return
        }
        sleep(milliseconds: pollMilliseconds)
    }
    throw DriverFailure(phase: "window_timeout", message: "The signed application did not expose an AX window")
}

private func activateApplication(_ running: NSRunningApplication,
                                 accessibilityElement application: AXUIElement,
                                 timeoutMilliseconds: Int,
                                 pollMilliseconds: Int) -> Bool {
    if running.activate(options: [.activateAllWindows]) {
        return true
    }

    _ = AXUIElementSetAttributeValue(application, kAXFrontmostAttribute as CFString, kCFBooleanTrue)
    if let windows = elementArrayAttribute(application, kAXWindowsAttribute as CFString) {
        for window in windows {
            _ = AXUIElementPerformAction(window, kAXRaiseAction as CFString)
        }
    }

    let limit = deadline(afterMilliseconds: min(timeoutMilliseconds, 3_000))
    while monotonicNow() < limit {
        if booleanAttribute(application, kAXFrontmostAttribute as CFString) {
            return true
        }
        sleep(milliseconds: pollMilliseconds)
    }
    return false
}

private func enterPath(_ path: String,
                       in application: AXUIElement,
                       timeoutMilliseconds: Int,
                       pollMilliseconds: Int) throws -> UInt64 {
    try postKey(37, flags: .maskCommand) // Command-L
    let focusLimit = deadline(afterMilliseconds: min(timeoutMilliseconds, 3_000))
    while monotonicNow() < focusLimit {
        if focusedTextInput(in: application) != nil {
            break
        }
        sleep(milliseconds: pollMilliseconds)
    }
    guard focusedTextInput(in: application) != nil else {
        throw DriverFailure(phase: "address_focus_timeout",
                            message: "Command-L did not focus an accessible path text field")
    }

    try postKey(0, flags: .maskCommand) // Command-A
    try postUnicodeText(path)
    let inputAt = monotonicNow()
    try postKey(36) // Return
    return inputAt
}

private func waitForFirstRow(in application: AXUIElement,
                             targetPrefix: String,
                             timeoutMilliseconds: Int,
                             pollMilliseconds: Int) throws -> TimedRowObservation {
    let limit = deadline(afterMilliseconds: timeoutMilliseconds)
    var polls = 0
    var nodesExamined = 0
    while monotonicNow() < limit {
        polls += 1
        let scan = scanRows(in: application, targetPrefix: targetPrefix)
        nodesExamined += scan.nodesExamined
        if let row = scan.rows.first {
            return TimedRowObservation(row: row,
                                       observedAt: monotonicNow(),
                                       pollCount: polls,
                                       nodesExamined: nodesExamined)
        }
        if scan.truncated {
            throw DriverFailure(phase: "ax_tree_limit",
                                message: "AX traversal exceeded 4096 nodes before finding a target row")
        }
        sleep(milliseconds: pollMilliseconds)
    }
    throw DriverFailure(phase: "first_row_timeout",
                        message: "No AX row with identifier '\(targetRowIdentifier)' and prefix '\(targetPrefix)' appeared")
}

private func waitForRowsToDisappear(in application: AXUIElement,
                                    targetPrefix: String,
                                    timeoutMilliseconds: Int,
                                    pollMilliseconds: Int) throws {
    let limit = deadline(afterMilliseconds: timeoutMilliseconds)
    while monotonicNow() < limit {
        let scan = scanRows(in: application, targetPrefix: targetPrefix)
        if scan.rows.isEmpty {
            return
        }
        if scan.truncated {
            throw DriverFailure(phase: "ax_tree_limit",
                                message: "AX traversal exceeded 4096 nodes while retiring stale target rows")
        }
        sleep(milliseconds: pollMilliseconds)
    }
    throw DriverFailure(phase: "stale_rows_timeout",
                        message: "Target rows were already visible and did not retire after staging navigation")
}

private func waitForSelectionToggle(in application: AXUIElement,
                                    targetPrefix: String,
                                    label: String,
                                    initialSelected: Bool,
                                    timeoutMilliseconds: Int,
                                    pollMilliseconds: Int) throws -> TimedRowObservation {
    let limit = deadline(afterMilliseconds: min(timeoutMilliseconds, 5_000))
    var polls = 0
    var nodesExamined = 0
    while monotonicNow() < limit {
        polls += 1
        let scan = scanRows(in: application, targetPrefix: targetPrefix)
        nodesExamined += scan.nodesExamined
        if let row = scan.rows.first(where: { $0.label == label && $0.selected != initialSelected }) {
            return TimedRowObservation(row: row,
                                       observedAt: monotonicNow(),
                                       pollCount: polls,
                                       nodesExamined: nodesExamined)
        }
        if scan.truncated {
            throw DriverFailure(phase: "ax_tree_limit",
                                message: "AX traversal exceeded 4096 nodes while observing selection")
        }
        sleep(milliseconds: pollMilliseconds)
    }
    throw DriverFailure(phase: "selection_timeout",
                        message: "Command-click did not toggle AXSelected for '\(label)'")
}

private func ensureRunningApplication(at appURL: URL) async throws -> NSRunningApplication {
    let expectedPath = normalizedPath(appURL)
    let running = NSRunningApplication.runningApplications(withBundleIdentifier: expectedBundleIdentifier)
    if let exact = running.first(where: { application in
        guard let bundleURL = application.bundleURL else { return false }
        return normalizedPath(bundleURL) == expectedPath
    }) {
        return exact
    }
    if let foreign = running.first {
        throw DriverFailure(phase: "application_identity",
                            message: "Bundle ID is already running from '\(foreign.bundleURL?.path ?? "unknown")', not '\(expectedPath)'")
    }

    let configuration = NSWorkspace.OpenConfiguration()
    configuration.activates = true
    configuration.addsToRecentItems = false
    let launched = try await withCheckedThrowingContinuation {
        (continuation: CheckedContinuation<NSRunningApplication, Error>) in
        NSWorkspace.shared.openApplication(at: appURL, configuration: configuration) { application, error in
            if let application {
                continuation.resume(returning: application)
            }
            else {
                continuation.resume(throwing: error ?? DriverFailure(
                    phase: "application_launch", message: "NSWorkspace returned no application"))
            }
        }
    }
    guard launched.bundleIdentifier == expectedBundleIdentifier,
          let bundleURL = launched.bundleURL,
          normalizedPath(bundleURL) == expectedPath else {
        throw DriverFailure(phase: "application_identity",
                            message: "NSWorkspace launched a different application identity or path")
    }
    return launched
}

private func writeJSONLine(_ record: MeasurementRecord, to outputURL: URL) throws {
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
    var data = try encoder.encode(record)
    data.append(0x0A)

    let fileManager = FileManager.default
    if !fileManager.fileExists(atPath: outputURL.path) {
        guard fileManager.createFile(atPath: outputURL.path, contents: nil) else {
            throw DriverFailure(phase: "output", message: "Unable to create \(outputURL.path)")
        }
    }
    let handle = try FileHandle(forWritingTo: outputURL)
    defer { try? handle.close() }
    try handle.seekToEnd()
    try handle.write(contentsOf: data)
    try handle.synchronize()
    FileHandle.standardOutput.write(data)
}

private func failureRecord(options: Options, pid: Int32?, failure: DriverFailure) -> MeasurementRecord {
    MeasurementRecord(schemaVersion: 1,
                      status: "fail",
                      timestampUTC: iso8601Now(),
                      bundleIdentifier: expectedBundleIdentifier,
                      appPath: options.appURL.path,
                      fixturePath: options.fixtureURL.path,
                      targetPrefix: options.targetPrefix,
                      pid: pid,
                      navigationInputUptimeNanoseconds: nil,
                      firstRowUptimeNanoseconds: nil,
                      firstRowLatencyMilliseconds: nil,
                      firstRowLabel: nil,
                      selectionInputUptimeNanoseconds: nil,
                      selectionObservedUptimeNanoseconds: nil,
                      selectionLatencyMilliseconds: nil,
                      selectionLabel: nil,
                      selectionInputKind: nil,
                      firstRowPollCount: nil,
                      selectionPollCount: nil,
                      axNodesExamined: nil,
                      failurePhase: failure.phase,
                      error: failure.message)
}

@main
private struct Q110SignedAXDriver {
    static func main() async {
        var parsedOptions: Options?
        var measuredPID: Int32?
        do {
            let options = try parseOptions(CommandLine.arguments)
            parsedOptions = options

            guard Bundle(url: options.appURL)?.bundleIdentifier == expectedBundleIdentifier else {
                throw DriverFailure(phase: "application_identity",
                                    message: "The app bundle does not have bundle ID \(expectedBundleIdentifier)")
            }
            guard AXIsProcessTrusted() else {
                throw DriverFailure(phase: "accessibility_permission",
                                    message: "The driver process is not trusted for Accessibility")
            }

            let running = try await ensureRunningApplication(at: options.appURL)
            measuredPID = running.processIdentifier
            guard !running.isTerminated else {
                throw DriverFailure(phase: "application_launch", message: "The application terminated during launch")
            }

            let launchLimit = deadline(afterMilliseconds: options.timeoutMilliseconds)
            while !running.isFinishedLaunching && !running.isTerminated && monotonicNow() < launchLimit {
                sleep(milliseconds: options.pollMilliseconds)
            }
            guard running.isFinishedLaunching && !running.isTerminated else {
                throw DriverFailure(phase: "application_launch_timeout",
                                    message: "The signed application did not finish launching")
            }

            let application = AXUIElementCreateApplication(running.processIdentifier)
            try waitForWindow(in: application,
                              timeoutMilliseconds: options.timeoutMilliseconds,
                              pollMilliseconds: options.pollMilliseconds)
            guard activateApplication(running,
                                      accessibilityElement: application,
                                      timeoutMilliseconds: options.timeoutMilliseconds,
                                      pollMilliseconds: options.pollMilliseconds) else {
                throw DriverFailure(phase: "application_activation", message: "Unable to activate the signed application")
            }

            let initialScan = scanRows(in: application, targetPrefix: options.targetPrefix)
            if initialScan.truncated {
                throw DriverFailure(phase: "ax_tree_limit",
                                    message: "Initial AX traversal exceeded 4096 nodes")
            }
            if !initialScan.rows.isEmpty {
                let stagingPath = options.fixtureURL.deletingLastPathComponent().path
                _ = try enterPath(stagingPath,
                                  in: application,
                                  timeoutMilliseconds: options.timeoutMilliseconds,
                                  pollMilliseconds: options.pollMilliseconds)
                try waitForRowsToDisappear(in: application,
                                           targetPrefix: options.targetPrefix,
                                           timeoutMilliseconds: options.timeoutMilliseconds,
                                           pollMilliseconds: options.pollMilliseconds)
            }

            let navigationInputAt = try enterPath(options.fixtureURL.path,
                                                  in: application,
                                                  timeoutMilliseconds: options.timeoutMilliseconds,
                                                  pollMilliseconds: options.pollMilliseconds)
            let first = try waitForFirstRow(in: application,
                                            targetPrefix: options.targetPrefix,
                                            timeoutMilliseconds: options.timeoutMilliseconds,
                                            pollMilliseconds: options.pollMilliseconds)

            guard let position = pointAttribute(first.row.element, kAXPositionAttribute as CFString),
                  let size = sizeAttribute(first.row.element, kAXSizeAttribute as CFString),
                  size.width > 0.0,
                  size.height > 0.0 else {
                throw DriverFailure(phase: "selection_precondition",
                                    message: "The first target AX row has no usable screen geometry")
            }
            guard running.activate(options: [.activateAllWindows]) else {
                throw DriverFailure(phase: "application_activation",
                                    message: "Unable to reactivate the application before selection")
            }
            let clickPoint = CGPoint(x: position.x + min(size.width * 0.5, 120.0),
                                     y: position.y + size.height * 0.5)
            let selectionInputAt = monotonicNow()
            try postCommandClick(at: clickPoint)
            let selection = try waitForSelectionToggle(in: application,
                                                       targetPrefix: options.targetPrefix,
                                                       label: first.row.label,
                                                       initialSelected: first.row.selected,
                                                       timeoutMilliseconds: options.timeoutMilliseconds,
                                                       pollMilliseconds: options.pollMilliseconds)

            let record = MeasurementRecord(
                schemaVersion: 1,
                status: "pass",
                timestampUTC: iso8601Now(),
                bundleIdentifier: expectedBundleIdentifier,
                appPath: options.appURL.path,
                fixturePath: options.fixtureURL.path,
                targetPrefix: options.targetPrefix,
                pid: running.processIdentifier,
                navigationInputUptimeNanoseconds: navigationInputAt,
                firstRowUptimeNanoseconds: first.observedAt,
                firstRowLatencyMilliseconds: milliseconds(from: navigationInputAt, to: first.observedAt),
                firstRowLabel: first.row.label,
                selectionInputUptimeNanoseconds: selectionInputAt,
                selectionObservedUptimeNanoseconds: selection.observedAt,
                selectionLatencyMilliseconds: milliseconds(from: selectionInputAt, to: selection.observedAt),
                selectionLabel: selection.row.label,
                selectionInputKind: "command_click_ax_selected_toggle",
                firstRowPollCount: first.pollCount,
                selectionPollCount: selection.pollCount,
                axNodesExamined: first.nodesExamined + selection.nodesExamined,
                failurePhase: nil,
                error: nil)
            try writeJSONLine(record, to: options.outputURL)
            exit(EXIT_SUCCESS)
        }
        catch let failure as DriverFailure {
            if let options = parsedOptions {
                do {
                    try writeJSONLine(failureRecord(options: options, pid: measuredPID, failure: failure),
                                      to: options.outputURL)
                }
                catch {
                    fputs("q1_10_signed_ax_driver: unable to write failure JSONL: \(error)\n", stderr)
                }
            }
            fputs("q1_10_signed_ax_driver: [\(failure.phase)] \(failure.message)\n", stderr)
            exit(EXIT_FAILURE)
        }
        catch {
            let failure = DriverFailure(phase: "unexpected", message: String(describing: error))
            if let options = parsedOptions {
                try? writeJSONLine(failureRecord(options: options, pid: measuredPID, failure: failure),
                                   to: options.outputURL)
            }
            fputs("q1_10_signed_ax_driver: [unexpected] \(error)\n", stderr)
            exit(EXIT_FAILURE)
        }
    }
}
