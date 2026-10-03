package top.niunaijun.blackboxa.view.main

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.net.VpnService
import android.os.Bundle
import android.util.Log
import android.view.Menu
import android.view.MenuItem
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.edit
import androidx.viewpager2.widget.ViewPager2
import com.afollestad.materialdialogs.MaterialDialog
import com.afollestad.materialdialogs.input.input
import top.niunaijun.blackbox.BlackBoxCore
import top.niunaijun.blackboxa.R
import top.niunaijun.blackboxa.app.App
import top.niunaijun.blackboxa.app.AppManager
import top.niunaijun.blackboxa.databinding.ActivityMainBinding
import top.niunaijun.blackboxa.util.Resolution
import top.niunaijun.blackboxa.util.inflate
import top.niunaijun.blackboxa.view.apps.AppsFragment
import top.niunaijun.blackboxa.view.base.LoadingActivity
import top.niunaijun.blackboxa.view.fake.FakeManagerActivity
import top.niunaijun.blackboxa.view.list.ListActivity
import top.niunaijun.blackboxa.view.setting.SettingActivity

class MainActivity : LoadingActivity() {

    private val viewBinding: ActivityMainBinding by inflate()

    private lateinit var mViewPagerAdapter: ViewPagerAdapter

    private var instanceIds = emptyList<Int>()
    private var instanceNames = emptyMap<Int, String>()

    private var currentUser = 0

    companion object {
        private const val TAG = "MainActivity"
        private const val STORAGE_PERMISSION_REQUEST_CODE = 1001
        private const val VPN_PERMISSION_REQUEST_CODE = 1002

        fun start(context: Context) {
            val intent = Intent(context, MainActivity::class.java)
            context.startActivity(intent)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        try {
            super.onCreate(savedInstanceState)

            try {
                BlackBoxCore.get().onBeforeMainActivityOnCreate(this)
            } catch (e: Exception) {
                Log.e(TAG, "Error in onBeforeMainActivityOnCreate: ${e.message}")
            }

            setContentView(viewBinding.root)
            initToolbar(viewBinding.toolbarLayout.toolbar, R.string.app_name)
            initViewPager()
            initFab()
            initToolbarSubTitle()

            
            checkStoragePermission()

            
            checkVpnPermission()

            try {
                BlackBoxCore.get().onAfterMainActivityOnCreate(this)
            } catch (e: Exception) {
                Log.e(TAG, "Error in onAfterMainActivityOnCreate: ${e.message}")
            }
        } catch (e: Exception) {
            Log.e(TAG, "Critical error in onCreate: ${e.message}")
            
            showErrorDialog("Failed to initialize app: ${e.message}")
        }
    }

    private fun checkStoragePermission() {
        try {
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
                
                if (!android.os.Environment.isExternalStorageManager()) {
                    Log.w(TAG, "MANAGE_EXTERNAL_STORAGE permission not granted")
                    showStoragePermissionDialog()
                }
            } else {
                
                if (androidx.core.content.ContextCompat.checkSelfPermission(
                                this,
                                android.Manifest.permission.WRITE_EXTERNAL_STORAGE
                        ) != android.content.pm.PackageManager.PERMISSION_GRANTED ||
                                androidx.core.content.ContextCompat.checkSelfPermission(
                                        this,
                                        android.Manifest.permission.READ_EXTERNAL_STORAGE
                                ) != android.content.pm.PackageManager.PERMISSION_GRANTED
                ) {
                    Log.w(
                            TAG,
                            "Storage permissions not granted on Android ${android.os.Build.VERSION.SDK_INT}"
                    )
                    requestLegacyStoragePermission()
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error checking storage permission: ${e.message}")
        }
    }

    private fun requestLegacyStoragePermission() {
        try {
            androidx.core.app.ActivityCompat.requestPermissions(
                    this,
                    arrayOf(
                            android.Manifest.permission.READ_EXTERNAL_STORAGE,
                            android.Manifest.permission.WRITE_EXTERNAL_STORAGE
                    ),
                    STORAGE_PERMISSION_REQUEST_CODE
            )
        } catch (e: Exception) {
            Log.e(TAG, "Error requesting storage permission: ${e.message}")
        }
    }

    override fun onRequestPermissionsResult(
            requestCode: Int,
            permissions: Array<out String>,
            grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == STORAGE_PERMISSION_REQUEST_CODE) {
            if (grantResults.isNotEmpty() &&
                            grantResults.all {
                                it == android.content.pm.PackageManager.PERMISSION_GRANTED
                            }
            ) {
                Log.d(TAG, "Storage permissions granted")
            } else {
                Log.w(TAG, "Storage permissions denied")
            }
        }
    }

    private fun showStoragePermissionDialog() {
        try {
            MaterialDialog(this).show {
                title(text = "Storage Permission Required")
                message(
                        text =
                                "This app needs 'All Files Access' permission to properly run sandboxed apps. Without this permission, some apps may not work correctly.\n\nPlease grant permission in the next screen."
                )
                positiveButton(text = "Grant Permission") { openAllFilesAccessSettings() }
                negativeButton(text = "Later") { Log.w(TAG, "User postponed storage permission") }
                cancelable(false)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error showing storage permission dialog: ${e.message}")
        }
    }

    private fun openAllFilesAccessSettings() {
        try {
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
                val intent =
                        Intent(
                                android.provider.Settings
                                        .ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION
                        )
                intent.data = Uri.parse("package:$packageName")
                storagePermissionResult.launch(intent)
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error opening storage settings: ${e.message}")
            
            try {
                val intent =
                        Intent(android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)
                storagePermissionResult.launch(intent)
            } catch (e2: Exception) {
                Log.e(TAG, "Error opening fallback storage settings: ${e2.message}")
            }
        }
    }

    private val storagePermissionResult =
            registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
                try {
                    if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
                        if (android.os.Environment.isExternalStorageManager()) {
                            Log.d(TAG, "Storage permission granted!")
                        } else {
                            Log.w(TAG, "Storage permission still not granted")
                        }
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Error handling storage permission result: ${e.message}")
                }
            }

    
    private fun checkVpnPermission() {
        try {
            val vpnIntent = VpnService.prepare(this)
            if (vpnIntent != null) {
                
                Log.d(TAG, "VPN permission not granted, requesting...")
                vpnPermissionResult.launch(vpnIntent)
            } else {
                
                Log.d(TAG, "VPN permission already granted")
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error checking VPN permission: ${e.message}")
        }
    }

    private val vpnPermissionResult =
            registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
                try {
                    if (result.resultCode == RESULT_OK) {
                        Log.d(TAG, "VPN permission granted!")
                        
                    } else {
                        Log.w(TAG, "VPN permission denied by user")
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Error handling VPN permission result: ${e.message}")
                }
            }

    private fun showErrorDialog(message: String) {
        try {
            MaterialDialog(this).show {
                title(text = "Error")
                message(text = message)
                positiveButton(text = "OK") { finish() }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error showing error dialog: ${e.message}")
            finish()
        }
    }

    private fun initToolbarSubTitle() {
        try {
            refreshInstances()
            viewBinding.toolbarLayout.toolbar.getChildAt(1)?.setOnClickListener {
                try {
                    val currentName = instanceNames[currentUser] ?: "Instance $currentUser"
                    MaterialDialog(this).show {
                        title(res = R.string.userRemark)
                        input(
                                hintRes = R.string.userRemark,
                                prefill = currentName
                        ) { _, input ->
                            val renamed = BlackBoxCore.get().renameUser(currentUser, input.toString())
                            if (renamed) {
                                refreshInstances()
                            } else {
                                Log.w(TAG, "Instance rename rejected: userId=$currentUser")
                            }
                        }
                        positiveButton(res = R.string.done)
                        negativeButton(res = R.string.cancel)
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Error showing remark dialog: ${e.message}")
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error in initToolbarSubTitle: ${e.message}")
        }
    }

    private fun initViewPager() {
        try {
            refreshInstances()
            mViewPagerAdapter = ViewPagerAdapter(this)
            mViewPagerAdapter.replaceData(instanceIds)
            viewBinding.viewPager.adapter = mViewPagerAdapter
            viewBinding.dotsIndicator.setViewPager2(viewBinding.viewPager)
            viewBinding.viewPager.registerOnPageChangeCallback(
                    object : ViewPager2.OnPageChangeCallback() {
                        override fun onPageSelected(position: Int) {
                            try {
                                super.onPageSelected(position)
                                if (position !in instanceIds.indices) return
                                currentUser = instanceIds[position]
                                updateUserRemark(currentUser)
                                showFloatButton(true)
                            } catch (e: Exception) {
                                Log.e(TAG, "Error in onPageSelected: ${e.message}")
                            }
                        }
                    }
            )
        } catch (e: Exception) {
            Log.e(TAG, "Error in initViewPager: ${e.message}")
        }
    }

    private fun initFab() {
        try {
            viewBinding.fab.setOnClickListener {
                try {
                    if (currentUser < 0) {
                        MaterialDialog(this@MainActivity).show {
                            title(res = R.string.instance_required_title)
                            message(res = R.string.instance_required_message)
                            positiveButton(res = R.string.done)
                        }
                        return@setOnClickListener
                    }
                    val userId = currentUser
                    val intent = Intent(this, ListActivity::class.java)
                    intent.putExtra("userID", userId)
                    apkPathResult.launch(intent)
                } catch (e: Exception) {
                    Log.e(TAG, "Error launching ListActivity: ${e.message}")
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error in initFab: ${e.message}")
        }
    }

    fun showFloatButton(show: Boolean) {
        try {
            val tranY: Float = Resolution.convertDpToPixel(120F, App.getContext())
            val time = 200L
            if (show) {
                viewBinding.fab.animate().translationY(0f).alpha(1f).setDuration(time).start()
            } else {
                viewBinding.fab.animate().translationY(tranY).alpha(0f).setDuration(time).start()
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error in showFloatButton: ${e.message}")
        }
    }

    fun scanUser() {
        try {
            runOnUiThread {
                val selectedUser = currentUser
                refreshInstances()
                mViewPagerAdapter.replaceData(instanceIds)
                val selectedPosition = instanceIds.indexOf(selectedUser)
                if (selectedPosition >= 0) {
                    viewBinding.viewPager.setCurrentItem(selectedPosition, false)
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error in scanUser: ${e.message}")
        }
    }

    private fun refreshInstances() {
        val users = BlackBoxCore.get().users.sortedBy { it.id }
        instanceIds = users.map { it.id }
        instanceNames = users.associate { it.id to (it.name ?: "Instance ${it.id}") }
        if (currentUser !in instanceIds) {
            currentUser = instanceIds.firstOrNull() ?: -1
        }
        updateUserRemark(currentUser)
    }

    private fun createInstance() {
        try {
            val nextId = (instanceIds.maxOrNull() ?: -1) + 1
            val created = BlackBoxCore.get().createUser(nextId)
            if (created == null) {
                Log.e(TAG, "Unable to create virtual instance id=$nextId")
                return
            }
            currentUser = created.id
            scanUser()
            val position = instanceIds.indexOf(created.id)
            if (position >= 0) viewBinding.viewPager.setCurrentItem(position, false)
        } catch (e: Exception) {
            Log.e(TAG, "Instance creation failed", e)
        }
    }

    private fun deleteCurrentInstance() {
        if (currentUser < 0) return
        val name = instanceNames[currentUser] ?: "Instance $currentUser"
        MaterialDialog(this).show {
            title(text = getString(R.string.instance_delete_title, name))
            message(res = R.string.instance_delete_message)
            positiveButton(res = R.string.done) {
                try {
                    BlackBoxCore.get().deleteUser(currentUser)
                    val deletedId = currentUser
                    AppManager.mRemarkSharedPreferences.edit {
                        remove("Remark$deletedId")
                        remove("AppList$deletedId")
                    }
                    currentUser = -1
                    scanUser()
                } catch (e: Exception) {
                    Log.e(TAG, "Instance deletion failed: id=$currentUser", e)
                }
            }
            negativeButton(res = R.string.cancel)
        }
    }

    private fun updateUserRemark(userId: Int) {
        try {
            viewBinding.toolbarLayout.toolbar.subtitle =
                    if (userId < 0) "No instance selected" else instanceNames[userId] ?: "Instance $userId"
        } catch (e: Exception) {
            Log.e(TAG, "Error updating user remark: ${e.message}")
            viewBinding.toolbarLayout.toolbar.subtitle = if (userId < 0) "No instance selected" else "Instance $userId"
        }
    }

    private val apkPathResult =
            registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
                try {
                    if (it.resultCode == RESULT_OK) {
                        it.data?.let { data ->
                            val userId = data.getIntExtra("userID", -1)
                            val source = data.getStringExtra("source")
                            val fragmentPosition = instanceIds.indexOf(userId)
                            val fragment = if (fragmentPosition >= 0) {
                                supportFragmentManager.fragments
                                        .filterIsInstance<AppsFragment>()
                                        .firstOrNull { it.userID == userId }
                            } else null
                            if (source != null && fragment != null) {
                                fragment.installApk(source)
                            }
                        }
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Error handling APK path result: ${e.message}")
                }
            }

    override fun onCreateOptionsMenu(menu: Menu?): Boolean {
        try {
            menuInflater.inflate(R.menu.menu_main, menu)
            return true
        } catch (e: Exception) {
            Log.e(TAG, "Error creating options menu: ${e.message}")
            return false
        }
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        try {
            when (item.itemId) {
                R.id.main_git -> {
                    val intent =
                            Intent(
                                    Intent.ACTION_VIEW,
                                    Uri.parse("https://github.com/ALEX5402/NewBlackbox")
                            )
                    startActivity(intent)
                }
                R.id.main_setting -> {
                    SettingActivity.start(this)
                }
                R.id.instance_create -> {
                    createInstance()
                }
                R.id.instance_delete -> {
                    deleteCurrentInstance()
                }
                R.id.main_tg -> {
                    val intent = Intent(Intent.ACTION_VIEW, Uri.parse("https://t.me/newblackboxa"))
                    startActivity(intent)
                }
                R.id.fake_location -> {
                    
                    val intent = Intent(this, FakeManagerActivity::class.java)
                    intent.putExtra("userID", currentUser.coerceAtLeast(0))
                    startActivity(intent)
                }
            }

            return true
        } catch (e: Exception) {
            Log.e(TAG, "Error handling menu item selection: ${e.message}")
            return false
        }
    }
}
