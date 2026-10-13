import Foundation
import SwiftUI

/// Sends a log to a computer on this network, found rather than typed wherever possible.
///
/// **Nothing is compiled in.** An address baked into a build is one person's network: it is wrong for
/// everybody else, and it says where that person's machine was. A build meant to be handed to anyone
/// must carry neither, so the address is *discovered* -- sweep this device's own /24 for the
/// receiver's port and take the first machine that answers as a receiver -- remembered once found,
/// and typed by hand only when discovery finds nothing.
///
/// Deliberately shaped to be privacy-respecting:
///   * off until an address is found or entered, and it stays off until switched on;
///   * never automatic -- there is no send without a tap on this button;
///   * only ever to the one address shown on the button;
///   * nothing else is transmitted: no analytics, no identifiers, no third party.
///
/// The other end is `husk_log_receiver.py`, which answers the discovery probe and prints its own
/// address for the case where it cannot be found.

/// Finds `husk_log_receiver.py` on the local network.
///
/// Same shape as `ServeFolder`'s search for the file server, and for the same reason: addresses move,
/// and a typed one goes stale. The receiver answers `GET /log` with a marker, so a sweep of 254
/// addresses identifies it without guessing -- and the marker is what stops a different server that
/// merely happens to hold the port from being used.
enum LogReceiverFinder {
    /// The receiver's default port. A different one can still be typed by hand.
    static let port = 8765

    /// One short request, from anywhere: static and nonisolated, so a task group can run 254 of these
    /// at once without anything crossing an isolation boundary.
    nonisolated static func probe(_ address: String) async -> Bool {
        // Accept what the user might have typed (a full URL, or with a path) and reduce it to host:port.
        let host = address.split(separator: "/").first.map(String.init) ?? address
        guard let url = URL(string: "http://\(host)/log") else { return false }
        var request = URLRequest(url: url)
        request.timeoutInterval = 1.5
        request.cachePolicy = .reloadIgnoringLocalCacheData
        guard let (data, response) = try? await finderSession.data(for: request),
              let http = response as? HTTPURLResponse, http.statusCode == 200 else { return false }
        return String(decoding: data, as: UTF8.self).contains("husk-log-receiver")
    }

    /// Every address on this device's /24 at once. The first receiver to answer wins, and the rest are
    /// cancelled, so the whole search costs about as long as the slowest single miss.
    nonisolated static func sweep() async -> String? {
        guard let base = ServeFolder.subnetPrefix() else { return nil }
        return await withTaskGroup(of: String?.self) { group in
            for n in 1...254 {
                let candidate = "\(base).\(n):\(port)"
                group.addTask { await probe(candidate) ? candidate : nil }
            }
            for await answer in group {
                if let answer {
                    group.cancelAll()
                    return answer
                }
            }
            return nil
        }
    }
}

/// One session for every probe, as in `ServeFolder`: a connect to an address with nothing on it fails
/// in milliseconds, so the timeout only has to cover a slow one and 254 of them in flight still
/// finish in about a second. `waitsForConnectivity` must be false, or a dead address hangs instead of
/// failing fast.
private let finderSession: URLSession = {
    let config = URLSessionConfiguration.ephemeral
    config.timeoutIntervalForRequest = 1.5
    config.timeoutIntervalForResource = 4
    config.waitsForConnectivity = false
    config.allowsCellularAccess = false
    config.requestCachePolicy = .reloadIgnoringLocalCacheData
    config.urlCache = nil
    return URLSession(configuration: config)
}()

struct SendLogToPCButton: View {
    /// What to send: the crash report when the button is on that screen, and otherwise the live
    /// log -- which is what exists when a game hangs on "Starting" instead of crashing.
    var file: URL? = nil

    private var source: URL { file ?? HuskLog.logFileURL }

    /// Empty to begin with, deliberately: there is no address of anybody's compiled into this build.
    /// The last one found is remembered here, so only a moved computer costs a search.
    @AppStorage(HuskLog.SendKeys.address) private var address = ""
    @AppStorage(HuskLog.SendKeys.on) private var enabled = false
    /// The message that goes with the log. Persisted, because the moment worth describing is
    /// usually while the game is still on screen, and killing it to reach this screen loses it.
    @AppStorage(HuskLog.SendKeys.note) private var note = ""

    @State private var draft = ""
    @State private var editing = false
    @State private var finding = false
    @State private var busy = false
    @State private var status: String?
    @State private var failed = false

    /// The address as it will actually be used: scheme and path filled in, or empty.
    private var target: URL? {
        var s = address.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !s.isEmpty else { return nil }
        if !s.lowercased().hasPrefix("http://") && !s.lowercased().hasPrefix("https://") { s = "http://" + s }
        // A path the user typed is taken as meant -- the receiver can be asked for a token path
        // (`/log/<secret>`) -- and only a bare host gets `/log` appended.
        if let schemeEnd = s.range(of: "://") {
            let rest = s[schemeEnd.upperBound...]
            if !rest.contains("/") {
                s += s.hasSuffix("/") ? "log" : "/log"
            }
        }
        return URL(string: s)
    }

    private var displayHost: String {
        guard let t = target else { return "" }
        return "\(t.host ?? "")\(t.port.map { ":\($0)" } ?? "")"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            if target == nil || editing {
                TextField("192.168.1.10:8765", text: $draft)
                    .textFieldStyle(.roundedBorder)
                    .keyboardType(.URL)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                HStack(spacing: 10) {
                    Button("Save address") {
                        address = draft.trimmingCharacters(in: .whitespacesAndNewlines)
                        editing = false
                        status = address.isEmpty ? nil : "Address saved. Sending is off until you switch it on."
                        failed = false
                    }
                    .buttonStyle(.bordered)

                    Button {
                        Task { await find() }
                    } label: {
                        Label(finding ? "Looking…" : "Find my computer", systemImage: "wifi")
                    }
                    .buttonStyle(.bordered)
                    .disabled(finding)

                    if target != nil {
                        Button("Cancel") { editing = false }.buttonStyle(.bordered)
                    }
                }
            } else {
                Toggle("Allow sending logs to \(displayHost)", isOn: $enabled)
                    .font(.callout)
                HStack(spacing: 10) {
                    Button {
                        send()
                    } label: {
                        Label(busy ? "Sending…" : "Send log to \(displayHost)", systemImage: "paperplane")
                            .frame(maxWidth: .infinity).padding(.vertical, 6)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(!enabled || busy)

                    Button("Change") { draft = address; editing = true }
                        .buttonStyle(.bordered)

                    Button {
                        Task { await find() }
                    } label: {
                        Image(systemName: "wifi")
                    }
                    .buttonStyle(.bordered)
                    .disabled(finding)
                }
            }

            VStack(alignment: .leading, spacing: 4) {
                Text("Message with this log (optional)").font(.caption).foregroundStyle(.secondary)
                TextField("What you saw, or what to look at", text: $note, axis: .vertical)
                    .lineLimit(2...5)
                    .textFieldStyle(.roundedBorder)
                    .font(.callout)
            }

            if let status {
                Text(status)
                    .font(.footnote)
                    .foregroundStyle(failed ? Color.red : Color.secondary)
            }

            Text("Sent only when you tap this, only to that address, and only over your own network. Nothing else leaves the phone.")
                .font(.footnote).foregroundStyle(.secondary)
        }
        .onAppear { if draft.isEmpty { draft = address } }
        // Nobody should have to know an address, so look for the receiver the first time this is
        // opened. Once found it is remembered, and this costs one request.
        .task { if address.isEmpty { await find() } }
    }

    /// Look for the receiver on this Wi-Fi.
    ///
    /// The remembered address is tried first: if the computer is where it was, that answers in
    /// milliseconds and nothing is swept. Only a machine that moved pays for the sweep.
    private func find() async {
        finding = true
        defer { finding = false }

        if !address.isEmpty, await LogReceiverFinder.probe(address), let host = target?.host, !host.isEmpty {
            status = "\(displayHost) is answering."
            failed = false
            return
        }
        if let found = await LogReceiverFinder.sweep() {
            address = found
            status = "Found the receiver at \(found)."
            failed = false
        } else {
            status = "No log receiver answered on this Wi-Fi. Run husk_log_receiver.py on the computer, "
                   + "or type its address."
            failed = true
        }
    }

    private func send() {
        guard let url = target else { return }
        guard let body = try? Data(contentsOf: source) else {
            status = "The log could not be read."; failed = true; return
        }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.timeoutInterval = 30
        request.setValue(buildStamp, forHTTPHeaderField: "X-Husk-Build")
        request.setValue("text/plain; charset=utf-8", forHTTPHeaderField: "Content-Type")
        // The message rides in a header as base64 of its UTF-8: HTTP headers carry no charset of
        // their own, and a message the user typed can contain any character they can type. The
        // receiver puts it at the top of the saved log, so it is read with the log rather than
        // existing as a second file nobody knows to open.
        let message = note.trimmingCharacters(in: .whitespacesAndNewlines)
        if !message.isEmpty {
            request.setValue(Data(message.utf8).base64EncodedString(), forHTTPHeaderField: "X-Husk-Note")
        }
        busy = true; status = nil; failed = false
        Task {
            do {
                let (_, response) = try await URLSession.shared.upload(for: request, from: body)
                let code = (response as? HTTPURLResponse)?.statusCode ?? 0
                await MainActor.run {
                    busy = false
                    failed = code != 200
                    status = failed ? "The receiver answered \(code)."
                                    : "Sent \(body.count) bytes to \(displayHost)"
                                      + (message.isEmpty ? "." : " with your message.")
                    if !failed { note = "" }     // a message is consumed by the send that carried it
                }
            } catch {
                await MainActor.run {
                    busy = false; failed = true
                    status = "Could not reach \(displayHost): \(error.localizedDescription)"
                }
            }
        }
    }

    private var buildStamp: String {
        let v = (Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String) ?? "husk"
        let b = (Bundle.main.infoDictionary?["CFBundleVersion"] as? String) ?? "0"
        return "\(v)-\(b)"
    }
}
