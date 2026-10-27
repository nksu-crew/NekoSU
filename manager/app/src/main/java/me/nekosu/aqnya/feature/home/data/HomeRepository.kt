package me.nekosu.aqnya.feature.home.data

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import me.nekosu.aqnya.BuildConfig
import me.nekosu.aqnya.core.Ncore
import me.nekosu.aqnya.data.installer.NcoreBoot
import me.nekosu.aqnya.data.kernel.KernelInfo
import me.nekosu.aqnya.feature.home.domain.HomeSnapshot
import me.nekosu.aqnya.util.getAppVersion

class HomeRepository(
    private val context: Context,
) {
    suspend fun load(): HomeSnapshot =
        withContext(Dispatchers.IO) {
            val installed = Ncore.ctl(1) == 0
            val managerVersion = getAppVersion(context)
            val isGki = KernelInfo.isGki()

            if (!installed) {
                return@withContext HomeSnapshot(
                    installed = false,
                    versionMatched = true,
                    managerVersion = managerVersion,
                    isGki = isGki,
                )
            }

            Ncore.ctl(3)
            val moduleVersion = runCatching { Ncore.moduleVersion() }.getOrNull()
            val expectedVersion = BuildConfig.NKSU_VERSION
            val versionMatched =
                moduleVersion.isNullOrBlank() ||
                    moduleVersion == "unknown" ||
                    expectedVersion == "unknown" ||
                    moduleVersion == expectedVersion

            if (versionMatched) {
                NcoreBoot.install(context)
            }

            HomeSnapshot(
                installed = true,
                versionMatched = versionMatched,
                managerVersion = managerVersion,
                isGki = isGki,
            )
        }

    suspend fun refresh() {
        withContext(Dispatchers.IO) {
            Ncore.ctl(3)
        }
    }
}
