package top.niunaijun.blackboxa.view.setting

import android.app.AlertDialog
import android.graphics.Typeface
import android.os.Bundle
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
            val file = File(requireContext().filesDir, "crash_logs/guest_events.jsonl")
            val recentEvents = readRecentEvents(file)
            val content = TextView(requireContext()).apply {
                text = recentEvents
                typeface = Typeface.MONOSPACE
                textSize = 12f
                setTextIsSelectable(false)
                setPadding(24, 16, 24, 16)
                gravity = Gravity.START
            }
            val scroll = ScrollView(requireContext()).apply { addView(content) }
            AlertDialog.Builder(requireContext())
                    .setTitle("Recent guest diagnostics")
                    .setView(scroll)
                    .setPositiveButton(android.R.string.ok, null)
                    .show()
            true
        }
    }

    private fun readRecentEvents(file: File): String {
        val output = StringBuilder()
        output.append("Guest lifecycle timeline\n")
        output.append(if (file.isFile) readTail(file, 48 * 1024) else "No guest lifecycle events recorded yet.\n")

        val crashDirectory = file.parentFile
        val crashReports = crashDirectory?.listFiles { candidate ->
            candidate.isFile && candidate.name.startsWith("crash_") && candidate.name.endsWith(".log")
        }?.sortedByDescending { it.lastModified() }.orEmpty()
        for (report in crashReports.take(3)) {
            if (output.length >= 96 * 1024) break
            output.append("\n--- Java crash report: ").append(report.name).append(" ---\n")
            output.append(readTail(report, 16 * 1024)).append('\n')
        }
        return output.toString()
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
