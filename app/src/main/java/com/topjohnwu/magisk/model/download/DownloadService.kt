package com.topjohnwu.magisk.model.download

import android.Manifest
import android.annotation.SuppressLint
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.os.Build
import android.webkit.MimeTypeMap
import android.widget.Toast
import androidx.annotation.RequiresPermission
import androidx.core.app.NotificationCompat
import com.topjohnwu.magisk.ClassMap
import com.topjohnwu.magisk.Config
import com.topjohnwu.magisk.Const
import com.topjohnwu.magisk.R
import com.topjohnwu.magisk.model.entity.internal.Configuration.*
import com.topjohnwu.magisk.model.entity.internal.Configuration.Flash.Secondary
import com.topjohnwu.magisk.model.entity.internal.DownloadSubject
import com.topjohnwu.magisk.model.entity.internal.DownloadSubject.Magisk
import com.topjohnwu.magisk.model.entity.internal.DownloadSubject.Module
import com.topjohnwu.magisk.ui.flash.FlashActivity
import com.topjohnwu.magisk.utils.Utils
import com.topjohnwu.magisk.utils.provide
import java.io.File
import kotlin.random.Random.Default.nextInt

/* More of a facade for [RemoteFileService], but whatever... */
@SuppressLint("Registered")
open class DownloadService : RemoteFileService() {

    private val context get() = this
    private val String.downloadsFile get() = Config.downloadsFile()?.let { File(it, this) }
    private val File.type
        get() = MimeTypeMap.getSingleton()
            .getMimeTypeFromExtension(extension)
            .orEmpty()

    override fun onFinished(file: File, subject: DownloadSubject) = when (subject) {
        is Magisk -> onFinishedInternal(file, subject)
        is Module -> onFinishedInternal(file, subject)
        else -> Unit
    }

    private fun onFinishedInternal(
        file: File,
        subject: Magisk
    ) = when (val conf = subject.configuration) {
        Download -> moveToDownloads(file)
        Uninstall -> FlashActivity.uninstall(this, file)
        is Patch -> FlashActivity.patch(this, file, conf.fileUri)
        is Flash -> FlashActivity.flash(this, file, conf is Secondary)
        else -> Unit
    }

    private fun onFinishedInternal(
        file: File,
        subject: Module
    ) = when (subject.configuration) {
        Download -> moveToDownloads(file)
        is Flash -> FlashActivity.install(this, file)
        else -> Unit
    }

    // ---

    override fun NotificationCompat.Builder.addActions(
        file: File,
        subject: DownloadSubject
    ) = when (subject) {
        is Magisk -> addActionsInternal(file, subject)
        is Module -> addActionsInternal(file, subject)
        else -> this
    }

    private fun NotificationCompat.Builder.addActionsInternal(
        file: File,
        subject: Magisk
    ) = when (val conf = subject.configuration) {
        Download -> setContentIntent(fileIntent(subject.fileName))
        Uninstall -> setContentIntent(FlashActivity.uninstallIntent(context, file))
        is Flash -> setContentIntent(FlashActivity.flashIntent(context, file, conf is Secondary))
        is Patch -> setContentIntent(FlashActivity.patchIntent(context, file, conf.fileUri))
        else -> this
    }

    private fun NotificationCompat.Builder.addActionsInternal(
        file: File,
        subject: Module
    ) = when (subject.configuration) {
        Download -> setContentIntent(fileIntent(subject.fileName))
        is Flash -> setContentIntent(FlashActivity.installIntent(context, file))
        else -> this
    }

    @Suppress("ReplaceSingleLineLet")
    private fun NotificationCompat.Builder.setContentIntent(intent: Intent) =
        PendingIntent.getActivity(context, nextInt(), intent, PendingIntent.FLAG_ONE_SHOT)
            .let { setContentIntent(it) }

    // ---

    private fun moveToDownloads(file: File) {
        val destination = file.name.downloadsFile ?: let {
            Utils.toast(
                getString(R.string.download_file_folder_error),
                Toast.LENGTH_LONG
            )
            return
        }

        if (file != destination) {
            destination.deleteRecursively()
            file.copyTo(destination)
        }

        Utils.toast(
            getString(
                R.string.internal_storage,
                "/" + destination.toRelativeString(Const.EXTERNAL_PATH.parentFile)
            ),
            Toast.LENGTH_LONG
        )
    }

    private fun fileIntent(fileName: String): Intent {
        val file = fileName.downloadsFile ?: let {
            Utils.toast(
                getString(R.string.download_file_folder_error),
                Toast.LENGTH_LONG
            )
            return Intent()
        }
        return Intent(Intent.ACTION_VIEW)
            .setDataAndType(file.provide(this), file.type)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    }

    class Builder {
        lateinit var subject: DownloadSubject
    }

    companion object {

        @RequiresPermission(allOf = [Manifest.permission.READ_EXTERNAL_STORAGE, Manifest.permission.WRITE_EXTERNAL_STORAGE])
        inline operator fun invoke(context: Context, argBuilder: Builder.() -> Unit) {
            val builder = Builder().apply(argBuilder)
            val intent = Intent(context, ClassMap[DownloadService::class.java])
                .putExtra(ARG_URL, builder.subject)

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }

    }

}