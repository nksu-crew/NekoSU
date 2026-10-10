package me.nekosu.aqnya.ui.component

import androidx.compose.foundation.focusable
import androidx.compose.foundation.layout.Box
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.input.key.KeyEvent
import androidx.compose.ui.input.key.onKeyEvent

/**
 * 抢占焦点并消费匹配的按键，移植自 KernelSU 的同名组件。
 *
 * 安装 / action 时，模块脚本常要求用户按音量键选择（脚本自己从 /dev/input 读键）。
 * 这里只是让前台 Activity 消费掉音量键，避免系统音量条弹出盖住日志界面；
 * 按键事件仍会到达 /dev/input，脚本照常能读到。
 */
@Composable
fun KeyEventBlocker(predicate: (KeyEvent) -> Boolean) {
    val requester = remember { FocusRequester() }
    Box(
        Modifier
            .onKeyEvent { predicate(it) }
            .focusRequester(requester)
            .focusable(),
    )
    LaunchedEffect(Unit) {
        requester.requestFocus()
    }
}
