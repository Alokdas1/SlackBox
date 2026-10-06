package top.niunaijun.blackboxa.view.setting

import android.app.AlertDialog
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Typeface
import android.os.Bundle
import android.os.Environment
import android.view.Gravity
import android.widget.ScrollView
import android.widget.TextView
import java.io.File
import java.io.RandomAccessFile
import androidx.preference.Preference
import androidx.preference.PreferenceFragmentCompat
import top.niunaijun.blackbox.BlackBoxCore
import top.niunaijun.blackboxa.R
import top.niunaijun.blackboxa.app.AppManager
import top.niunaijun.blackboxa.util.toast
import top.niunaijun.blackboxa.view.gms.GmsManagerActivity

class SettingFragment : PreferenceFragmentCompat() {

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        setPreferencesFromResource(R.xml.setting, rootKey)

        initGms()

        invalidHideState {
            val rootHidePreference: Preference = (findPreference("root_hide")!!)
            val hideRoot = AppManager.mBlackBoxLoader.hideRoot()
            rootHidePreference.setDefaultValue(hideRoot)
            rootHidePreference
        }

        invalidHideState {
            val daemonPreference: Preference = (findPreference("daemon_enable")!!)
            val mDaemonEnable = AppManager.mBlackBoxLoader.daemonEnable()
            daemonPreference.setDefaultValue(mDaemonEnable)
            daemonPreference
        }

        invalidHideState {
            val vpnPreference: Preference = (findPreference("use_vpn_network")!!)
            val mUseVpnNetwork = AppManager.mBlackBoxLoader.useVpnNetwork()
            vpnPreference.setDefaultValue(mUseVpnNetwork)
            vpnPreference
        }

        invalidHideState {
            val disableFlagSecurePreference: Preference = (findPreference("disable_flag_secure")!!)
            val mDisableFlagSecure = AppManager.mBlackBoxLoader.disableFlagSecure()
            disableFlagSecurePreference.setDefaultValue(mDisableFlagSecure)
            disableFlagSecurePreference
        }

        initSendLogs()
        initGuestDiagnostics()
    }

    private fun initGms() {
        val gmsManagerPreference: Preference = (findPreference("gms_manager")!!)

        if (BlackBoxCore.get().isSupportGms) {

            gmsManagerPreference.setOnPreferenceClickListener {
                GmsManagerActivity.start(requireContext())
                true
            }
        } else {
            gmsManagerPreference.summary = getString(R.string.no_gms)
            gmsManagerPreference.isEnabled = false
        }
    }

    private fun invalidHideState(block: () -> Preference) {
        val pref = block()
        pref.setOnPreferenceChangeListener { preference, newValue ->
            val tmpHide = (newValue == true)
            when (preference.key) {
                "root_hide" -> {

                    AppManager.mBlackBoxLoader.invalidHideRoot(tmpHide)
                }
                "daemon_enable" -> {
                    AppManager.mBlackBoxLoader.invalidDaemonEnable(tmpHide)
                }
                "use_vpn_network" -> {
                    AppManager.mBlackBoxLoader.invalidUseVpnNetwork(tmpHide)
                }
                "disable_flag_secure" -> {
                    AppManager.mBlackBoxLoader.invalidDisableFlagSecure(tmpHide)
                }
            }

            toast(R.string.restart_module)
            return@setOnPreferenceChangeListener true
        }
    }
    private fun initSendLogs() {
        val sendLogsPreference: Preference? = findPreference("send_logs")
        sendLogsPreference?.setOnPreferenceClickListener {
            it.isEnabled = false
            BlackBoxCore.get()
                    .sendLogs(
                            "Manual Log Upload from Settings",
                            true,
                            object : BlackBoxCore.LogSendListener {
                                override fun onSuccess() {
                                    activity?.runOnUiThread { sendLogsPreference.isEnabled = true }
                                }

                                override fun onFailure(error: String?) {
                                    activity?.runOnUiThread { sendLogsPreference.isEnabled = true }
                                }
                            }
                    )
            toast("Sending logs... (Check notifications for status)")
            true
        }
    }

    private fun initGuestDiagnostics() {
        findPreference<Preference>("view_guest_diagnostics")?.setOnPreferenceClickListener {
            // Never let a diagnostics failure produce a blank dialog -- a broken
            // diagnostics screen is worse than a noisy one when it is the only
            // window into a death.
            val report = try {
                buildGuestDiagnosticsReport()
            } catch (error: Throwable) {
                "Guest diagnostics failed: ${error.javaClass.simpleName}: ${error.message}"
            }
            val content = TextView(requireContext()).apply {
                text = report
                typeface = Typeface.MONOSPACE
                textSize = 12f
                setTextIsSelectable(false)
                setPadding(24, 16, 24, 16)
                gravity = Gravity.START
            }
            val scroll = ScrollView(requireContext()).apply { addView(content) }
            AlertDialog.Builder(requireContext())
                    .setTitle("Guest diagnostics")
                    .setView(scroll)
                    .setNeutralButton("Copy") { _, _ ->
                        val clipboard = requireContext()
                                .getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager
                        if (clipboard != null) {
                            clipboard.setPrimaryClip(
                                    ClipData.newPlainText("SlackBox guest diagnostics", report)
                            )
                            toast("Guest diagnostics copied")
                        } else {
                            toast("Clipboard unavailable")
                        }
                    }
                    .setPositiveButton(android.R.string.ok, null)
                    .show()
            true
        }
    }

    /**
     * Deep scan over every diagnostic surface the container produces.
     *
     * Reliability matters more than tidiness here: a guest that dies with a native
     * signal leaves its record outside the Java crash path, so the timeline alone
     * is not enough. This reads the lifecycle timeline, counts and classifies it,
     * and pulls both native and Java reports from the internal directory and the
     * shared mirror (the internal one can be unreadable to an external collector).
     */
    private fun buildGuestDiagnosticsReport(): String {
        val crashDirectory = File(requireContext().filesDir, "crash_logs")
        val externalCrash = requireContext().getExternalFilesDir("crash_logs")
        val legacyShared = File(Environment.getExternalStorageDirectory(), "Download/logs")
        val timelineFile = File(crashDirectory, "guest_events.jsonl")
        val timeline = if (timelineFile.isFile) readTail(timelineFile, 64 * 1024) else ""

        // Three roots, because the shared mirror moved. Before this change the
        // native handler wrote to Download/logs, which needs
        // MANAGE_EXTERNAL_STORAGE and silently failed when it was not granted
        // (the device logcat shows exactly that). It now prefers
        // getExternalFilesDir("crash_logs"), so an older crash file may still be
        // sitting in Download/logs and a newer one in external files. Collect
        // from all three and keep the newest copy of each name.
        val roots = listOf(
                crashDirectory to "internal",
                externalCrash to "external",
                legacyShared to "legacy-shared")
        val nativeReports = collectDiagnosticFiles(roots, "native_crash_", ".log")
        val javaReports = collectDiagnosticFiles(roots, "crash_", ".log")

        val output = StringBuilder()
        output.append("=== Guest diagnostics ===\n")
        output.append("Search roots:\n")
        for ((directory, label) in roots) {
            val path = directory?.absolutePath ?: "(unavailable)"
            val state = when {
                directory == null -> "unavailable"
                directory.isDirectory -> "ok"
                else -> "MISSING"
            }
            output.append("  [").append(state).append("] ").append(label)
                    .append(": ").append(path).append('\n')
        }
        output.append(summarizeTimeline(timeline))
        output.append(artifactList("Native crash reports", nativeReports))
        output.append(artifactList("Java crash reports", javaReports))

        output.append("\n=== Lifecycle timeline ===\n")
        output.append(timeline.ifBlank { "No guest lifecycle events recorded yet." })

        appendReportBodies(output, "Native", nativeReports, 3, 24 * 1024)
        appendReportBodies(output, "Java", javaReports, 3, 16 * 1024)

        output.append("\n=== logcat -d (last lines, in-process) ===\n")
        output.append(captureLogcatDump(4000))
        return output.toString()
    }

    /**
     * In-process logcat slice, appended to the report so the crash moment is
     * visible without a separate adb capture.
     *
     * The external capture script only covered the first ~2s of a 51s run on the
     * device this was written for, so it missed the abort entirely. This reads the
     * ring buffer that is already in memory at the moment the user opens the
     * dialog, which is after the death.
     *
     * "logcat -d" with a negative tail is not supported by every build, so the
     * line count is passed with -t and the result is trimmed here instead.
     */
    private fun captureLogcatDump(maxLines: Int): String {
        return try {
            val process = Runtime.getRuntime().exec(
                    arrayOf("logcat", "-d", "-v", "threadtime", "-t", maxLines.toString()))
            process.errorStream.close()
            val text = process.inputStream.bufferedReader().readText()
            process.waitFor()
            process.destroy()
            val keys = listOf(
                    "fatal", "androidruntime", "sigsegv", "sigabrt", "sigbus",
                    "tombstone", "backtrace", "crash_dump", "abort",
                    "naijun", "blackbox", "slackbox", "nativecore",
                    "guest_process", "libc", "DEBUG")
            val filtered = text.lineSequence().filter { line ->
                val lower = line.lowercase()
                keys.any { lower.contains(it) }
            }.joinToString("\n")
            if (filtered.isBlank()) text.takeLast(12_000) else filtered
        } catch (t: Throwable) {
            "logcat dump failed: ${t.javaClass.simpleName}: ${t.message}"
        }
    }

    // Flat JSON lines: pull one quoted field without dragging in a JSON parser.
    private fun jsonField(line: String, key: String): String? {
        val marker = "\"$key\":\""
        val start = line.indexOf(marker)
        if (start < 0) return null
        val valueStart = start + marker.length
        val valueEnd = line.indexOf('"', valueStart)
        if (valueEnd < 0) return null
        return line.substring(valueStart, valueEnd)
    }

    private fun summarizeTimeline(timeline: String): String {
        if (timeline.isBlank()) return "Deep scan: no events.\n"
        val counts = LinkedHashMap<String, Int>()
        val exits = ArrayList<String>()
        for (line in timeline.lineSequence()) {
            val event = jsonField(line, "event") ?: continue
            counts[event] = (counts[event] ?: 0) + 1
            // Exact match only: a bare contains("crash") also matched
            // native_crash_handler_armed and reported phantom exits.
            val isExit = event == "guest_process_binder_died" ||
                    event.endsWith("_crash") || event == "crash"
            if (isExit) {
                exits.add("  - " + decodeExit(jsonField(line, "detail").orEmpty()))
            }
        }
        val output = StringBuilder()
        output.append("Deep scan: ").append(counts.values.sum())
        output.append(" events, ").append(counts.size).append(" types\n")
        for ((event, count) in counts.entries.sortedByDescending { it.value }) {
            output.append("  ").append(event).append(": ").append(count).append('\n')
        }
        if (exits.isNotEmpty()) {
            output.append("Process exits:\n")
            exits.forEach { output.append(it).append('\n') }
        }
        return output.toString()
    }

    // The platform reports SIGNALED plus a bare number; name it so the reader does
    // not have to remember that 7 is SIGBUS.
    private fun decodeExit(detail: String): String {
        val reason = Regex("reason=([A-Z_]+)").find(detail)?.groupValues?.get(1)
        val status = Regex("status=(-?\\d+)").find(detail)?.groupValues?.get(1)?.toIntOrNull()
        return if (reason == "SIGNALED" && status != null) {
            "SIGNALED -> ${signalName(status)} (signal $status)"
        } else {
            reason ?: "unspecified"
        }
    }

    private fun signalName(signal: Int): String = when (signal) {
        4 -> "SIGILL"
        5 -> "SIGTRAP"
        6 -> "SIGABRT"
        7 -> "SIGBUS"
        8 -> "SIGFPE"
        9 -> "SIGKILL"
        11 -> "SIGSEGV"
        13 -> "SIGPIPE"
        15 -> "SIGTERM"
        else -> "signal"
    }

    /**
     * Collect prefix*suffix files across every supplied root, keeping the newest
     * copy of any given filename. Roots are (directory, label) pairs so the report
     * can say which one a file actually came from.
     */
    private fun collectDiagnosticFiles(roots: List<Pair<File?, String>>,
                                       prefix: String, suffix: String): List<Pair<File, String>> {
        val found = LinkedHashMap<String, Pair<File, String>>()
        for ((directory, label) in roots) {
            if (directory == null || !directory.isDirectory) continue
            val matches = directory.listFiles { file ->
                file.isFile && file.name.startsWith(prefix) && file.name.endsWith(suffix)
            }.orEmpty()
            for (match in matches) {
                val existing = found[match.name]
                if (existing == null || match.lastModified() > existing.first.lastModified()) {
                    found[match.name] = match to label
                }
            }
        }
        return found.values.sortedByDescending { it.first.lastModified() }
    }

    private fun artifactList(title: String, reports: List<Pair<File, String>>): String {
        val output = StringBuilder()
        output.append('\n').append(title).append(": ").append(reports.size).append('\n')
        if (reports.isEmpty()) {
            output.append("  (none)\n")
            return output.toString()
        }
        for ((file, origin) in reports) {
            output.append("  - ").append(file.name)
            output.append(" [").append(origin).append(", ").append(file.length() / 1024).append(" KB]\n")
        }
        return output.toString()
    }

    private fun appendReportBodies(output: StringBuilder, label: String,
                                   reports: List<Pair<File, String>>, limit: Int, maxBytes: Int) {
        for ((file, origin) in reports.take(limit)) {
            if (output.length >= 256 * 1024) break
            output.append("\n--- ").append(label).append(" crash report: ").append(file.name)
            output.append(" (").append(origin).append(") ---\n")
            output.append(readTail(file, maxBytes)).append('\n')
        }
    }

    private fun readTail(file: File, maxBytes: Int): String = try {
        RandomAccessFile(file, "r").use { input ->
            val offset = (input.length() - maxBytes).coerceAtLeast(0)
            input.seek(offset)
            if (offset > 0) input.readLine()
            val bytes = ByteArray((input.length() - input.filePointer).coerceAtMost(maxBytes.toLong()).toInt())
            input.readFully(bytes)
            String(bytes, Charsets.UTF_8).trim()
        }
    } catch (error: Exception) {
        android.util.Log.e("SettingFragment", "Unable to read guest diagnostic file", error)
        "Unable to read ${file.name}: ${error.javaClass.simpleName}"
    }
}
