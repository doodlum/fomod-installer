using BUTR.NativeAOT.Shared;
using FomodInstaller.Interface;
using FomodInstaller.Interface.ui;
using System;
using System.Runtime.InteropServices;
using System.Threading.Tasks;

namespace ModInstaller.Native.Adapters;

internal class NativeCoreDelegatesHandler : CoreDelegates, IDisposable
{
    public static unsafe NativeCoreDelegatesHandler? FromPointer(void* ptr) => GCHandle.FromIntPtr(new IntPtr(ptr)).Target as NativeCoreDelegatesHandler;
    private readonly object _gate = new();
    private readonly CallbackPluginDelegates _pluginDelegates;
    private readonly CallbackContextDelegates _contextDelegates;
    private readonly CallbackIniDelegates _iniDelegates;
    private readonly CallbackUIDelegates _uiDelegates;
    private bool _closing;
    private bool _disposed;
    private int _active;
    private bool _publishing;
    private Task? _completion;
    public override PluginDelegates plugin => _pluginDelegates;
    public override IniDelegates ini => _iniDelegates;
    public override ContextDelegates context => _contextDelegates;
    public override UIDelegates ui => _uiDelegates;
    public unsafe param_ptr* OwnerPtr { get; }
    public unsafe VoidPtr* HandlePtr { get; }
    public unsafe NativeCoreDelegatesHandler(param_ptr* pOwner, CallbackPluginDelegates plugins,
        CallbackContextDelegates context, CallbackIniDelegates ini, CallbackUIDelegates ui)
    {
        _pluginDelegates = plugins;
        _contextDelegates = context;
        _iniDelegates = ini;
        _uiDelegates = ui;
        OwnerPtr = pOwner;
        HandlePtr = (VoidPtr*) GCHandle.ToIntPtr(GCHandle.Alloc(this)).ToPointer();
    }
    public bool TryBeginInstall()
    {
        lock (_gate)
        {
            if (_closing || _disposed || _active != 0) return false;
            _active++;
            _publishing = true;
            _completion = null;
            return true;
        }
    }
    public void EndInstall()
    {
        _uiDelegates.RevokeDialog();
        // The native completion callback has RETURNED before this decrement.
        lock (_gate) _active--;
    }
    public void TrackCompletion(Task completion)
    {
        lock (_gate) { _completion = completion; _publishing = false; }
    }
    public void AbortInstall()
    {
        _uiDelegates.RevokeDialog();
        lock (_gate) { _active--; _publishing = false; _completion = null; }
    }
    public bool IsIdle { get { lock (_gate) return _active == 0 && !_publishing && (_completion is null || _completion.IsCompleted); } }
    public void RequestClose()
    {
        lock (_gate) _closing = true;
        // May synchronously complete the task; never hold the admission gate.
        _uiDelegates.RequestClose();
    }
    public unsafe void Dispose()
    {
        lock (_gate)
        {
            if (_disposed) return;
            if (_active != 0 || _publishing || (_completion is not null && !_completion.IsCompleted)) throw new InvalidOperationException("Installer is still active");
            _closing = true;
            _disposed = true;
        }
        _uiDelegates.RequestClose();
        GCHandle.FromIntPtr(new IntPtr(HandlePtr)).Free();
    }
}
