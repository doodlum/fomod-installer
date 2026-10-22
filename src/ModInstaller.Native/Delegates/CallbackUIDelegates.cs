using BUTR.NativeAOT.Shared;
using FomodInstaller.Interface;
using FomodInstaller.Interface.ui;
using System;
using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

namespace ModInstaller.Native.Adapters;

internal class CallbackUIDelegates : UIDelegates
{
    private sealed class Dialog
    {
        public readonly object Gate = new();
        public required nint Id { get; init; }
        public required Action<int, int, int[]> Select { get; init; }
        public required Action<bool, int> Continue { get; init; }
        public required Action Cancel { get; init; }
        public bool Active = true;
    }
    private static long _nextId;
    private static readonly ConcurrentDictionary<nint, Dialog> Dialogs = new();
    private readonly object _gate = new();
    private bool _closing;
    private Dialog? _dialog;
    private readonly unsafe param_ptr* _pOwner;
    private readonly N_UI_StartDialog _startDialog;
    private readonly N_UI_EndDialog _endDialog;
    private readonly N_UI_UpdateState _updateState;
    private readonly N_UI_ReportError _reportError;
    public unsafe CallbackUIDelegates(param_ptr* owner, N_UI_StartDialog start,
        N_UI_EndDialog end, N_UI_UpdateState update, N_UI_ReportError report)
    {
        _pOwner = owner; _startDialog = start; _endDialog = end;
        _updateState = update; _reportError = report;
    }
    private Dialog? TakeDialog(bool close)
    {
        Dialog? dialog;
        lock (_gate)
        {
            if (close) _closing = true;
            dialog = _dialog;
            _dialog = null;
        }
        if (dialog is not null)
        {
            Dialogs.TryRemove(dialog.Id, out _);
            // Never nest _gate and the reply gate: an active reply can EndDialog.
            lock (dialog.Gate) dialog.Active = false;
        }
        return dialog;
    }
    public void RevokeDialog() => TakeDialog(false);
    public void RequestClose()
    {
        var dialog = TakeDialog(true);
        dialog?.Cancel();
    }
    public override unsafe void StartDialog(string moduleName, HeaderImage image,
        Action<int, int, int[]> select, Action<bool, int> cont, Action cancel)
    {
        var dialog = new Dialog { Id = checked((nint) Interlocked.Increment(ref _nextId)),
            Select = select, Continue = cont, Cancel = cancel };
        lock (_gate)
        {
            // Outside the native-call catch: a late dialog must never wait for JS.
            if (_closing) throw new OperationCanceledException("Installer is closing");
            if (_dialog is not null) throw new InvalidOperationException("Dialog is already active");
            Dialogs[dialog.Id] = dialog;
            _dialog = dialog;
        }
        fixed (char* name = moduleName)
        fixed (char* header = BUTR.NativeAOT.Shared.Utils.SerializeJson(image, Bindings.CustomSourceGenerationContext.HeaderImage))
        {
            try
            {
                using var result = SafeStructMallocHandle.Create(_startDialog(_pOwner,
                    (param_string*) name, (param_json*) header, (param_ptr*) dialog.Id,
                    &StartDialogSelectCallback, &StartDialogContinueCallback, &StartDialogCancelCallback), true);
                result.ValueAsVoid();
            }
            catch
            {
                RevokeDialog();
                cancel();
                throw;
            }
        }
    }
    private static unsafe void Reply(param_ptr* owner, return_value_void* pResult, Action<Dialog> action)
    {
        using var logger = LogCallbackMethod(pResult);
        // Own the reply allocation even when its ID was already revoked.
        using var result = SafeStructMallocHandle.Create(pResult, true);
        Dialog? dialog = null;
        try
        {
            result.ValueAsVoid();
            if (!Dialogs.TryGetValue((nint) owner, out dialog)) return;
            lock (dialog.Gate) { if (dialog.Active) action(dialog); }
        }
        catch (Exception e)
        {
            logger.LogException(e);
            // The action remains available even if EndDialog revoked its ID.
            try { dialog?.Cancel(); } catch (Exception cancelError) { logger.LogException(cancelError); }
        }
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe void StartDialogSelectCallback(param_ptr* owner, param_int step, param_int group, param_json* options, return_value_void* result)
        => Reply(owner, result, dialog => dialog.Select(step, group,
            BUTR.NativeAOT.Shared.Utils.DeserializeJson(options, Bindings.CustomSourceGenerationContext.Int32Array)));
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe void StartDialogContinueCallback(param_ptr* owner, param_bool forward, param_int step, return_value_void* result)
        => Reply(owner, result, dialog => dialog.Continue(forward, step));
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe void StartDialogCancelCallback(param_ptr* owner, return_value_void* result)
        => Reply(owner, result, dialog => dialog.Cancel());
    public override unsafe void EndDialog()
    {
        RevokeDialog();
        using var logger = LogMethod();
        try
        {
            using var result = SafeStructMallocHandle.Create(_endDialog(_pOwner), true);
            result.ValueAsVoid();
        }
        catch (Exception e) { logger.LogException(e); }
    }
    public override unsafe void UpdateState(InstallerStep[] steps, int current)
    {
        lock (_gate) { if (_closing) throw new OperationCanceledException("Installer is closing"); }
        fixed (char* json = BUTR.NativeAOT.Shared.Utils.SerializeJson(steps, Bindings.CustomSourceGenerationContext.InstallerStepArray))
        {
            using var result = SafeStructMallocHandle.Create(_updateState(_pOwner, (param_json*) json, current), true);
            result.ValueAsVoid();
        }
    }
    public override void ReportError(string title, string message, string details)
    {
#if DEBUG
        using var logger = LogMethod(title.ToFormattable(), message.ToFormattable(), details.ToFormattable());
#else
        using var logger = LogMethod();
#endif

        try
        {
            var tcs = new TaskCompletionSource();
            ReportErrorNative(title, message, details, tcs);
        }
        catch (Exception e)
        {
            logger.LogException(e);
            throw;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe void ReportErrorCallback(param_ptr* pOwner, return_value_void* pResult)
    {
#if DEBUG
        using var logger = LogCallbackMethod(pResult);
#else
        using var logger = LogCallbackMethod(pResult);
#endif

        try
        {

            if (pOwner == null)
            {
                logger.LogException(new ArgumentNullException(nameof(pOwner)));
                return;
            }

            if (GCHandle.FromIntPtr((IntPtr) pOwner) is not {Target: TaskCompletionSource tcs} handle)
            {
                logger.LogException(new InvalidOperationException("Invalid GCHandle."));
                return;
            }

            using var result = SafeStructMallocHandle.Create(pResult, true);
            logger.LogResult(result);
            result.SetAsVoid(tcs);
            handle.Free();
        }
        catch (Exception e)
        {
            logger.LogException(e);
            // Never unwind a managed exception through an unmanaged callback.
        }
    }

    private unsafe void ReportErrorNative(ReadOnlySpan<char> title, ReadOnlySpan<char> message, ReadOnlySpan<char> details, TaskCompletionSource tcs)
    {
#if DEBUG
        using var logger = LogMethod(title.ToString().ToFormattable(), message.ToString().ToFormattable(), details.ToString().ToFormattable());
#else
        using var logger = LogMethod();
#endif

        var handle = GCHandle.Alloc(tcs, GCHandleType.Normal);

        fixed (char* pTitle = title)
        fixed (char* pMessage = message)
        fixed (char* pDetails = details)
        {
            try
            {
                using var result = SafeStructMallocHandle.Create(_reportError(_pOwner, (param_string*) pTitle, (param_string*) pMessage, (param_string*) pDetails, (param_ptr*) GCHandle.ToIntPtr(handle), &ReportErrorCallback), true);
                logger.LogResult(result);
                result.ValueAsVoid();
            }
            catch (Exception e)
            {
                logger.LogException(e);
                tcs.TrySetException(e);
                handle.Free();
            }
        }
    }
}