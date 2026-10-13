/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright 2024-2025 Fcitx5 for Android Contributors
 */

package org.fcitx.fcitx5.android.input

import android.content.Context
import android.hardware.input.InputManager
import android.os.Build
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.inputmethod.EditorInfo
import org.fcitx.fcitx5.android.data.prefs.AppPrefs
import org.fcitx.fcitx5.android.input.candidates.floating.FloatingCandidatesMode
import org.fcitx.fcitx5.android.utils.inputManager
import org.fcitx.fcitx5.android.utils.isTypeNull

class InputDeviceManager(private val onChange: (Boolean) -> Unit) {

    private var inputView: InputView? = null
    private var candidatesView: CandidatesView? = null
    private var inputManager: InputManager? = null
    private var hasPhysicalKeyboard = false

    private fun isValidPhysicalKeyboard(device: InputDevice?): Boolean {
        if (device == null) return false
        return !device.isVirtual &&
            (Build.VERSION.SDK_INT < Build.VERSION_CODES.O_MR1 || device.isEnabled) &&
            device.keyboardType == InputDevice.KEYBOARD_TYPE_ALPHABETIC
    }

    private fun refreshPhysicalKeyboard(): Boolean {
        val im = inputManager ?: return false
        val hasPk = im.inputDeviceIds.any { id ->
            isValidPhysicalKeyboard(im.getInputDevice(id))
        }
        val changed = hasPhysicalKeyboard != hasPk
        hasPhysicalKeyboard = hasPk
        return changed
    }

    private fun updateModeOnDeviceChange() {
        isVirtualKeyboard = when (candidatesViewMode) {
            FloatingCandidatesMode.Disabled -> true
            else -> !hasPhysicalKeyboard
        }
    }

    private val inputDeviceListener = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) {
            if (refreshPhysicalKeyboard()) {
                updateModeOnDeviceChange()
            }
        }

        override fun onInputDeviceRemoved(deviceId: Int) {
            if (refreshPhysicalKeyboard()) {
                updateModeOnDeviceChange()
            }
        }

        override fun onInputDeviceChanged(deviceId: Int) {
            if (refreshPhysicalKeyboard()) {
                updateModeOnDeviceChange()
            }
        }
    }

    fun registerInputDeviceListener(context: Context) {
        val im = context.inputManager
        inputManager = im
        im.registerInputDeviceListener(inputDeviceListener, null)
        if (refreshPhysicalKeyboard()) {
            updateModeOnDeviceChange()
        }
    }

    fun unregisterInputDeviceListener() {
        inputManager?.unregisterInputDeviceListener(inputDeviceListener)
        inputManager = null
    }

    private fun setupInputViewEvents(isVirtual: Boolean) {
        val iv = inputView ?: return
        iv.handleEvents = isVirtual
        iv.visibility = if (isVirtual) View.VISIBLE else View.GONE
    }

    private fun setupCandidatesViewEvents(isVirtual: Boolean) {
        val cv = candidatesView ?: return
        cv.handleEvents = !isVirtual
        // hide CandidatesView when entering virtual keyboard mode,
        // but preserve the visibility when entering physical keyboard mode (in case it's empty)
        if (isVirtual) {
            cv.visibility = View.GONE
        }
    }

    private fun setupViewEvents(isVirtual: Boolean) {
        setupInputViewEvents(isVirtual)
        setupCandidatesViewEvents(isVirtual)
    }

    var isVirtualKeyboard = true
        private set(value) {
            if (field == value) {
                return
            }
            field = value
            setupViewEvents(value)
            // fire change AFTER updating InputView(s),
            // make the view(s) ready for incoming events during `onChange`
            onChange(value)
        }

    fun setInputView(inputView: InputView) {
        this.inputView = inputView
        setupInputViewEvents(this.isVirtualKeyboard)
    }

    fun setCandidatesView(candidatesView: CandidatesView) {
        this.candidatesView = candidatesView
        setupCandidatesViewEvents(this.isVirtualKeyboard)
    }

    var startedInputView = false
        private set
    private var isNullInputType = true

    private var candidatesViewMode by AppPrefs.getInstance().candidates.mode

    fun notifyOnStartInput(attribute: EditorInfo) {
        isNullInputType = attribute.isTypeNull()
    }

    /**
     * @return should use virtual keyboard
     */
    fun evaluateOnStartInputView(info: EditorInfo, service: FcitxInputMethodService): Boolean {
        isNullInputType = info.isTypeNull()
        refreshPhysicalKeyboard()
        isVirtualKeyboard = when (candidatesViewMode) {
            FloatingCandidatesMode.SystemDefault -> !hasPhysicalKeyboard && service.superEvaluateInputViewShown()
            FloatingCandidatesMode.InputDevice -> !hasPhysicalKeyboard
            FloatingCandidatesMode.Disabled -> true
        }
        startedInputView = true
        return isVirtualKeyboard
    }

    /**
     * @return should force show input views on hardware key down
     */
    fun evaluateOnKeyDown(e: KeyEvent, service: FcitxInputMethodService): Boolean {
        if (startedInputView) {
            // filter out back/home/volume buttons and combination keys
            if (e.unicodeChar != 0) {
                // evaluate virtual keyboard visibility when pressing physical keyboard while InputView visible
                evaluateOnKeyDownInner(service)
            }
            // no need to force show InputView since it's already visible
            return false
        } else {
            // force show InputView when focusing on text input (likely inputType is not TYPE_NULL)
            // and pressing any digit/letter/punctuation key on physical keyboard
            val showInputView = !isNullInputType && e.unicodeChar != 0
            if (showInputView) {
                evaluateOnKeyDownInner(service)
            }
            return showInputView
        }
    }

    private fun evaluateOnKeyDownInner(service: FcitxInputMethodService) {
        isVirtualKeyboard = when (candidatesViewMode) {
            FloatingCandidatesMode.SystemDefault -> !hasPhysicalKeyboard && service.superEvaluateInputViewShown()
            FloatingCandidatesMode.InputDevice -> false
            FloatingCandidatesMode.Disabled -> true
        }
    }

    fun evaluateOnViewClicked(service: FcitxInputMethodService) {
        if (!startedInputView) return
        isVirtualKeyboard = when (candidatesViewMode) {
            FloatingCandidatesMode.SystemDefault -> !hasPhysicalKeyboard && service.superEvaluateInputViewShown()
            FloatingCandidatesMode.InputDevice -> !hasPhysicalKeyboard
            FloatingCandidatesMode.Disabled -> true
        }
    }

    fun evaluateOnUpdateEditorToolType(toolType: Int, service: FcitxInputMethodService) {
        if (!startedInputView) return
        isVirtualKeyboard = when (candidatesViewMode) {
            FloatingCandidatesMode.SystemDefault -> !hasPhysicalKeyboard && service.superEvaluateInputViewShown()
            FloatingCandidatesMode.InputDevice ->
                // switch to virtual keyboard on touch screen events only when no physical keyboard is enabled
                if (hasPhysicalKeyboard) {
                    false
                } else if (toolType == MotionEvent.TOOL_TYPE_FINGER || toolType == MotionEvent.TOOL_TYPE_STYLUS) {
                    true
                } else {
                    isVirtualKeyboard
                }
            FloatingCandidatesMode.Disabled -> true
        }
    }

    /**
     * Should be called when input method switched **by user**
     * @return should force show inputView for [CandidatesView] when input method switched by user
     */
    fun evaluateOnInputMethodSwitch(): Boolean {
        return !isVirtualKeyboard && !startedInputView
    }

    /**
     * Should be called whenever active input method changes,
     * eg. focus in/out, editor capability flag changes, user presses the shortcut, and so on...
     * @return should update status icon for this change
     */
    fun evaluateOnInputMethodActivate(): Boolean {
        return startedInputView && !isVirtualKeyboard
    }

    fun onFinishInputView() {
        startedInputView = false
    }
}
