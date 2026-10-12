import Foundation
import SwiftUI

/// Where logs go by default: this machine, as it was when the build was made.
///
/// LiveContainer clears the app's data on every install, so a setting alone would have to be
/// typed again each time. The address of the machine being built for does not change between
/// builds, so it is compiled in as the starting value; "Change" in the UI overwrites it if the
/// machine moves or the network hands out a different address.
private let defaultLogAddress = "192.168.1.123:8765"

/// Sends a log to an address the user names, over their own network, when they tap the button.
///
/// Deliberately shaped to be privacy-respecting:
///   * off until an address is entered, and it stays off until switched on;
///   * never automatic -- there is no send without a tap on this button;
///   * only ever to the one address shown on the button, which the user typed;
///   * nothing else is transmitted: no analytics, no identifiers, no third party.
///
/// The other end is `husk_log_receiver.py`, which prints the address to type in here.
struct SendLogToPCButton: View {
    /// What to send: the crash report when the button is on that screen, and otherwise the live
    /// log -- which is what exists when a game hangs on "Starting" instead of crashing.
    var file: URL? = nil

    private var source: URL { file ?? HuskLog.logFileURL }

    @AppStorage(HuskLog.SendKeys.address) private var address = defaultLogAddress
    @AppStorage(HuskLog.SendKeys.on) private var enabled = false
    /// The message that goes with the log. Persisted, because the moment worth describing is
    /// usually while the game is still on screen, and killing it to reach this screen loses it.
    @AppStorage(HuskLog.SendKeys.note) private var note = ""

    @State private var draft = ""
    @State private var editing = false
    @State private var busy = false
    @State private var status: String?
    @State private var failed = false

    /// The address as it will actually be used: scheme and path filled in, or empty.
    private var target: URL? {
        var s = address.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !s.isEmpty else { return nil }
        if !s.lowercased().hasPrefix("http://") && !s.lowercased().hasPrefix("https://") { s = "http://" + s }
        if !s.hasSuffix("/log") { s += s.hasSuffix("/") ? "log" : "/log" }
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
