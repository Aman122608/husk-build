// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// What THIS build is for, keyed by the build's own commit.
///
/// The owner installs a new build every few minutes while testing and cannot
/// tell one from the next. The app already names its build at boot
/// (`[boot] build : 3.0.0 (129) 44fcd4a-dirty built ...`), so the same identity
/// keys a note: `Resources/BuildNotes.json` lists one entry per short commit,
/// and the entry for the running build is shown once when it first opens, then
/// re-readable from Settings.
enum BuildNotes {

    /// One build's note, as written in the file.
    struct Note: Identifiable, Decodable {
        let sha: String
        let date: String
        let title: String
        let goal: String
        let details: [String]
        var id: String { sha }
    }

    private struct File: Decodable {
        let notes: [Note]
    }

    // MARK: - which build this is

    /// The raw stamp Info.plist carries: "44fcd4a", "44fcd4a-dirty", "unstamped".
    ///
    /// package_ipa.sh stamps `HuskBuildCommit` with `git rev-parse --short HEAD`
    /// and appends "-dirty" when the tree it built from had uncommitted changes.
    static var stamp: String {
        (Bundle.main.infoDictionary?["HuskBuildCommit"] as? String) ?? ""
    }

    /// The seven hex digits of the stamp: "44fcd4a-dirty" -> "44fcd4a".
    /// The note file keys on this, so the suffix must not reach the lookup.
    static func short(_ raw: String) -> String {
        var hex = ""
        for ch in raw.lowercased() {
            guard ch.isHexDigit else { break }
            hex.append(ch)
            if hex.count == 7 { break }
        }
        return hex
    }

    /// The short commit this build was made from.
    static let current: String = short(stamp)

    /// What "seen" is remembered against: the short commit when the build named
    /// itself, otherwise the raw stamp. Empty when the build cannot be told apart
    /// from the next one, in which case there is nothing to show once.
    static var identity: String {
        if !current.isEmpty { return current }
        let raw = stamp
        return raw.isEmpty || raw == "unstamped" ? "" : raw
    }

    // MARK: - the notes

    /// The note for the running build, or nil when this build is unlisted.
    static var note: Note? { all.first { $0.sha == current } }

    /// Every note in the file, in the order the file lists them (newest first).
    static let all: [Note] = load()

    private static func load() -> [Note] {
        let url = Bundle.main.url(forResource: "BuildNotes", withExtension: "json")
            ?? Bundle.main.url(forResource: "BuildNotes", withExtension: "json", subdirectory: "Resources")
            ?? Bundle.main.path(forResource: "BuildNotes", ofType: "json", inDirectory: "Resources")
                .flatMap { URL(fileURLWithPath: $0) }
        guard let url,
              let data = try? Data(contentsOf: url),
              let file = try? JSONDecoder().decode(File.self, from: data) else {
            HuskLog.log("build", "notes: no readable BuildNotes.json in the bundle")
            return []
        }
        HuskLog.log("build", "notes: \(file.notes.count) entries, this build is \(current.isEmpty ? "unnamed" : current)")
        return file.notes
    }

    // MARK: - seen, once per build

    private static let seenKey = "husk.buildNotes.seen"

    /// Whether the note for this build should be shown on open: once per build.
    ///
    /// Tracked against the build's identity, not a version number, so the next
    /// build shows its note even though the version string has not changed.
    static func due() -> Bool {
        let id = identity
        guard !id.isEmpty else { return false }
        return UserDefaults.standard.string(forKey: seenKey) != id
    }

    static func markSeen() {
        let id = identity
        guard !id.isEmpty else { return }
        UserDefaults.standard.set(id, forKey: seenKey)
    }
}

// MARK: - the note itself

/// The note's words, drawn the same wherever it is shown: the sheet, and Settings.
struct BuildNotesBody: View {
    let note: BuildNotes.Note?
    let build: String

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            if let note = note {
                VStack(alignment: .leading, spacing: 5) {
                    Text(note.title)
                        .font(.system(size: 20, weight: .bold))
                        .foregroundStyle(Theme.text)
                    Text("Build \(build) · \(note.date)")
                        .font(.caption)
                        .foregroundStyle(Theme.textDim)
                }
                Text(note.goal)
                    .font(.system(size: 16))
                    .foregroundStyle(Theme.text)
                    .fixedSize(horizontal: false, vertical: true)
                VStack(alignment: .leading, spacing: 11) {
                    ForEach(note.details, id: \.self) { detail in
                        HStack(alignment: .top, spacing: 10) {
                            Circle()
                                .fill(Theme.accent)
                                .frame(width: 6, height: 6)
                                .padding(.top, 6)
                            Text(detail)
                                .font(.system(size: 15))
                                .foregroundStyle(Theme.textDim)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                }
            } else {
                unlisted
            }
        }
    }

    /// A build nobody wrote a note for. Said plainly rather than shown as a wrong
    /// note or as nothing at all.
    private var unlisted: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("No note for this build")
                .font(.system(size: 17, weight: .semibold))
                .foregroundStyle(Theme.text)
            Text(build.isEmpty
                 ? "This build did not stamp which commit it was made from, so there is nothing to look up."
                 : "Nobody has written a note for \(build) yet. Notes are written per build, keyed by the commit the build was made from.")
                .font(.system(size: 15))
                .foregroundStyle(Theme.textDim)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}

/// The note for the running build, once, when it first opens.
struct BuildNotesSheet: View {
    let note: BuildNotes.Note?
    let build: String
    var done: () -> Void

    var body: some View {
        VStack(spacing: 0) {
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    HStack(spacing: 8) {
                        Image(systemName: "sparkles")
                            .font(.system(size: 15, weight: .semibold))
                            .foregroundStyle(Theme.accent)
                        Text("WHAT THIS BUILD IS FOR")
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(Theme.textDim)
                        Spacer(minLength: 0)
                    }
                    BuildNotesBody(note: note, build: build)
                }
                .padding(22)
                .background(Theme.surface, in: RoundedRectangle(cornerRadius: Theme.rowCorner, style: .continuous))
                .overlay(RoundedRectangle(cornerRadius: Theme.rowCorner, style: .continuous)
                            .stroke(Theme.hairline, lineWidth: 0.5))
                .padding(.horizontal, 18)
                .padding(.top, 22)
            }
            Button(action: done) {
                Text("Got it")
                    .font(.headline)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 8)
            }
            .buttonStyle(.borderedProminent)
            .tint(Theme.accent)
            .padding(.horizontal, 18)
            .padding(.vertical, 16)
        }
        .background(Theme.bg.ignoresSafeArea())
    }
}

/// The same note, pushed from Settings so it can be read again after dismissal.
struct BuildNotesSettings: View {
    var body: some View {
        List {
            Section {
                BuildNotesBody(note: BuildNotes.note, build: BuildNotes.current)
                    .padding(.vertical, 8)
            }
            Section {
                DetailRow(label: "Build", value: BuildNotes.current.isEmpty ? "unnamed" : BuildNotes.current)
                DetailRow(label: "Stamped as", value: BuildNotes.stamp.isEmpty ? "—" : BuildNotes.stamp, mono: false)
                DetailRow(label: "Date", value: BuildNotes.note?.date ?? "—", mono: false)
            } footer: {
                Text("A note is shown once, the first time its build opens, and can be read again here. "
                   + "Notes are written per build, keyed by the commit the build was made from.")
            }
        }
        .navigationTitle("This Build")
        .navigationBarTitleDisplayMode(.inline)
    }
}
