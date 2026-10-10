// SPDX-License-Identifier: GPL-2.0-or-later
import Darwin
import Foundation
import SwiftUI

/// Finds the computer sharing a folder (tools/serve-folder.py) without anyone typing an address.
///
/// Two addresses move: the phone re-leases, and so can the computer, so a typed address goes stale the moment
/// the router feels like it. The server answers a plain HTTP request, so the app can look for it instead:
///
///   1. sweep this device's own /24 for port 8642, every address at once, `GET /list` with a short timeout;
///   2. the address that worked last time (remembered across launches);
///   3. the address compiled in below, as it was when this build was made.
///
/// Whichever answers is remembered, so the common case costs one request and no sweep.
///
/// Deliberately not built on the UDP announce the server also sends on 8766: receiving a broadcast needs
/// Apple's multicast entitlement (`com.apple.developer.networking.multicast`), which has to be requested from
/// Apple. A TCP connect needs only the local-network usage description already in Info.plist.
@MainActor
final class ServeFolder: ObservableObject {
    static let shared = ServeFolder()

    /// The port serve-folder.py listens on.
    static let port = 8642
    /// Where the computer was when this build was made. The last resort, not the plan.
    static let compiledIn = "192.168.1.123:8642"
    private static let rememberKey = "husk.serveFolder.host"

    struct Entry: Identifiable, Hashable {
        /// The path inside the served folder, exactly as the server reports it. This is what the file URL is built from.
        let path: String
        let size: Int64
        var id: String { path }
        var name: String { path.split(separator: "/").last.map(String.init) ?? path }
        /// The row's title: the file name without the .apk the user does not need to read.
        var title: String {
            let base = name
            return base.lowercased().hasSuffix(".apk") ? String(base.dropLast(4)) : base
        }
    }

    enum State: Equatable {
        case idle
        case searching
        case loading
        case ready
        case empty
        case notFound
    }

    @Published private(set) var state: State = .idle
    /// The computer that answered, as `host:port`.
    @Published private(set) var host: String?
    @Published private(set) var entries: [Entry] = []
    /// The name the server gives its folder, when it says.
    @Published private(set) var folderName: String = ""

    /// What worked last time. Tried before the compiled-in address, so a moved computer costs no sweep.
    private var remembered: String? {
        UserDefaults.standard.string(forKey: Self.rememberKey)
    }

    private init() {}

    // MARK: - finding it

    /// Sweep, then the remembered address, then the compiled-in one. Whichever answers is remembered and listed.
    func discover() async {
        guard state != .searching else { return }
        state = .searching
        entries = []
        HuskLog.log("discovery", "looking for the folder server: sweeping \(Self.subnetPrefix() ?? "?") for port \(Self.port)")

        if let found = await sweep() {
            await adopt(found, how: "the sweep")
            return
        }
        HuskLog.log("discovery", "the sweep found nothing")

        for candidate in [remembered, Self.compiledIn].compactMap({ $0 }) {
            if await Self.probe(candidate) {
                await adopt(candidate, how: "the last address that worked")
                return
            }
            HuskLog.log("discovery", "\(candidate) did not answer either")
        }

        host = nil
        state = .notFound
    }

    /// Every address on this device's /24 at once, short timeout each. The first to answer wins.
    private func sweep() async -> String? {
        guard let base = Self.subnetPrefix() else { return nil }
        return await withTaskGroup(of: String?.self) { group in
            for n in 1...254 {
                let candidate = "\(base).\(n):\(Self.port)"
                group.addTask { await ServeFolder.probe(candidate) ? candidate : nil }
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

    /// Take an address: remember it, then read what is in the folder.
    private func adopt(_ address: String, how: String) async {
        host = address
        UserDefaults.standard.set(address, forKey: Self.rememberKey)
        HuskLog.log("discovery", "folder server found at \(address) (via \(how))")
        await load()
    }

    /// Read the folder's listing and turn it into rows.
    ///
    /// The manifest is read here, but it is never handed to `Downloads`: a listing of files makes a `.folder`
    /// job, whose files land in Shared Storage and never appear in the library. Each row starts its own
    /// single-file job instead -- see `download(_:)`.
    func load() async {
        guard let host else { return }
        state = .loading
        guard let url = URL(string: "http://\(host)/") else { state = .notFound; return }
        var request = URLRequest(url: url)
        request.timeoutInterval = 10
        request.cachePolicy = .reloadIgnoringLocalCacheData
        guard let (data, response) = try? await probeSession.data(for: request),
              let http = response as? HTTPURLResponse, http.statusCode == 200,
              let manifest = try? JSONDecoder().decode(Manifest.self, from: data) else {
            HuskLog.log("discovery", "\(host) answered the sweep but not its listing")
            state = .notFound
            return
        }
        folderName = manifest.name
        entries = manifest.files.map { Entry(path: $0.path, size: $0.size) }
        state = entries.isEmpty ? .empty : .ready
        HuskLog.log("discovery", "\(host): \(entries.count) file(s) in \"\(folderName)\"")
    }

    // MARK: - downloading one row

    /// Start one file: `http://<host>/f/<exact path>`. This is the whole point -- a single-file URL is what
    /// becomes a real app in the library. Handing the app the manifest instead makes a folder job, which drops
    /// the files into Shared Storage and leaves the library empty.
    func download(_ entry: Entry) {
        guard let host, let url = Self.fileURL(host: host, path: entry.path) else { return }
        HuskLog.log("discovery", "downloading \(entry.path) as one file: \(url)")
        Downloads.shared.addFile(name: entry.name, url: url, size: entry.size)
    }

    /// The job already running (or done) for a row, so it can show progress instead of a second Get button.
    func job(for entry: Entry) -> Downloads.Job? {
        guard let host, let url = Self.fileURL(host: host, path: entry.path) else { return nil }
        return Downloads.shared.jobs.first { $0.source == url }
    }

    private static func fileURL(host: String, path: String) -> String? {
        let encoded = path.split(separator: "/")
            .map { $0.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? String($0) }
            .joined(separator: "/")
        guard !encoded.isEmpty else { return nil }
        return "http://\(host)/f/\(encoded)"
    }

    // MARK: - the network this device is on

    /// The first three octets of this device's Wi-Fi address, so the sweep is the /24 it is actually on.
    nonisolated static func subnetPrefix() -> String? {
        var addresses: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&addresses) == 0, let first = addresses else { return nil }
        defer { freeifaddrs(addresses) }
        var cursor = first
        while true {
            let interface = cursor.pointee
            if let sa = interface.ifa_addr, sa.pointee.sa_family == UInt8(AF_INET) {
                let name = String(cString: interface.ifa_name)
                var text = [CChar](repeating: 0, count: Int(NI_MAXHOST))
                if getnameinfo(sa, socklen_t(sa.pointee.sa_len), &text, socklen_t(text.count), nil, 0, NI_NUMERICHOST) == 0 {
                    let ip = String(cString: text)
                    // en0 is Wi-Fi. 169.254.x is what iOS hands out when it cannot reach a DHCP server, and
                    // sweeping that finds nothing.
                    if name.hasPrefix("en"), !ip.hasPrefix("127."), !ip.hasPrefix("169.254.") {
                        let parts = ip.split(separator: ".")
                        if parts.count == 4 { return parts.prefix(3).joined(separator: ".") }
                    }
                }
            }
            guard let next = interface.ifa_next else { break }
            cursor = next
        }
        return nil
    }

    // MARK: - the request that identifies the server

    private struct Manifest: Decodable {
        struct File: Decodable { let path: String; let size: Int64 }
        let name: String
        let files: [File]
    }

    /// One short request, from anywhere (the sweep runs 254 of these at once). Static and nonisolated: it is
    /// called from a task group and touches no instance state, so nothing crosses an isolation boundary.
    nonisolated static func probe(_ address: String) async -> Bool {
        guard let url = URL(string: "http://\(address)/list") else { return false }
        var request = URLRequest(url: url)
        request.timeoutInterval = 1.5
        request.cachePolicy = .reloadIgnoringLocalCacheData
        guard let (data, response) = try? await probeSession.data(for: request),
              let http = response as? HTTPURLResponse, http.statusCode == 200 else { return false }
        // /list always ends with the short-link block, and starts with a numbered file list. Either identifies
        // this server rather than something else that happens to be listening on 8642.
        let body = String(decoding: data, as: UTF8.self)
        return body.contains("short links") || body.contains("serve-folder") || body.hasPrefix("1. ")
    }
}

/// One session for every probe: a connect to an address with nothing on it fails in milliseconds, so the
/// timeout only has to cover a slow one, and 254 of them in flight still finish in about a second. A file-scope
/// constant rather than a static on the class, so a task-group probe reaches it without an isolation hop.
private let probeSession: URLSession = {
    let config = URLSessionConfiguration.ephemeral
    config.timeoutIntervalForRequest = 1.5
    config.timeoutIntervalForResource = 4
    config.waitsForConnectivity = false
    config.allowsCellularAccess = false
    config.requestCachePolicy = .reloadIgnoringLocalCacheData
    config.urlCache = nil
    return URLSession(configuration: config)
}()

// MARK: - the rows

/// What the computer on this network is sharing, in the Downloads tab.
struct LocalGamesSection: View {
    @ObservedObject private var serve = ServeFolder.shared
    @ObservedObject private var downloads = Downloads.shared

    var body: some View {
        Section {
            switch serve.state {
            case .idle, .searching:
                HStack(spacing: 10) {
                    ProgressView()
                    Text("Looking for your computer…").foregroundStyle(.secondary)
                }
            case .loading:
                HStack(spacing: 10) {
                    ProgressView()
                    Text("Reading the folder…").foregroundStyle(.secondary)
                }
            case .empty:
                Text("The folder on your computer is empty.").foregroundStyle(.secondary)
            case .notFound:
                VStack(alignment: .leading, spacing: 8) {
                    Text("No computer found").font(.headline)
                    Text("Run this on the computer sharing the games, on the same Wi‑Fi, and look again:\n"
                         + "python3 serve-folder.py <folder>")
                        .font(.footnote).foregroundStyle(.secondary)
                        .textSelection(.enabled)
                }
                .padding(.vertical, 4)
            case .ready:
                ForEach(serve.entries) { entry in row(entry) }
            }
        } header: {
            HStack {
                Text("On Your Network")
                Spacer()
                if let host = serve.host, serve.state == .ready || serve.state == .empty {
                    Text(host).font(.caption2).foregroundStyle(.secondary).textCase(nil)
                }
            }
        } footer: {
            VStack(alignment: .leading, spacing: 6) {
                if serve.state == .ready || serve.state == .empty {
                    Text(serve.folderName.isEmpty ? "Games shared from your computer."
                                                  : "Games in \"\(serve.folderName)\" on your computer.")
                } else if serve.state == .notFound {
                    Text("Husk sweeps this Wi‑Fi for the folder server. Nothing answered on port \(ServeFolder.port).")
                }
                Button {
                    Task { await serve.discover() }
                } label: {
                    Label("Look again", systemImage: "arrow.clockwise")
                }
                .font(.caption)
                .disabled(serve.state == .searching || serve.state == .loading)
            }
        }
        // Find it as soon as the tab is opened. No address to type, and nothing to press.
        .task { if serve.state == .idle { await serve.discover() } }
    }

    private func row(_ entry: ServeFolder.Entry) -> some View {
        let job = serve.job(for: entry)
        let running = job.map { !$0.complete && !$0.paused }
        return HStack(spacing: 12) {
            Image(systemName: "shippingbox.fill").foregroundStyle(Color.accentColor)
            VStack(alignment: .leading, spacing: 2) {
                Text(entry.title).font(.body.weight(.medium)).lineLimit(1)
                Text(subtitle(entry, job: job)).font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
            Spacer(minLength: 8)
            if let job, running == true {
                let fraction = job.total > 0 ? min(1, Double(job.received) / Double(job.total)) : 0
                ProgressView(value: fraction).frame(width: 54)
            } else if job?.complete == true {
                Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
            } else {
                Button("Get") { serve.download(entry) }
                    .buttonStyle(.borderedProminent)
                    .buttonBorderShape(.capsule)
            }
        }
        .contentShape(Rectangle())
        .onTapGesture { if job == nil { serve.download(entry) } }
    }

    private func subtitle(_ entry: ServeFolder.Entry, job: Downloads.Job?) -> String {
        if let job, job.complete { return "Added to the library" }
        if let job, !job.complete {
            return "\(Downloads.bytes(job.received)) of \(Downloads.bytes(job.total))"
        }
        return Downloads.bytes(entry.size)
    }
}
