import Foundation
import SwiftUI

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
    /// What to send, if there is anything: the crash report file, which carries the full log.
    let file: URL?

    @AppStorage(HuskLog.SendKeys.address) private var address = ""
    @AppStorage(HuskLog.SendKeys.on) private var enabled = false

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
                    .disabled(!enabled || busy || file == nil)

                    Button("Change") { draft = address; editing = true }
                        .buttonStyle(.bordered)
                }
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
        guard let url = target, let file else { return }
        guard let body = try? Data(contentsOf: file) else {
            status = "The log could not be read."; failed = true; return
        }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.timeoutInterval = 30
        request.setValue(buildStamp, forHTTPHeaderField: "X-Husk-Build")
        request.setValue("text/plain; charset=utf-8", forHTTPHeaderField: "Content-Type")
        busy = true; status = nil; failed = false
        Task {
            do {
                let (_, response) = try await URLSession.shared.upload(for: request, from: body)
                let code = (response as? HTTPURLResponse)?.statusCode ?? 0
                await MainActor.run {
                    busy = false
                    failed = code != 200
                    status = failed ? "The receiver answered \(code)." : "Sent \(body.count) bytes to \(displayHost)."
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
