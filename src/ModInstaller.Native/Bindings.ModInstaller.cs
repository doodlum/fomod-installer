using BUTR.NativeAOT.Shared;

using FomodInstaller.ModInstaller;

using ModInstaller.Lite;
using ModInstaller.Native.Adapters;

using System;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace ModInstaller.Native;

public static unsafe partial class Bindings
{
    [UnmanagedCallersOnly(EntryPoint = "create_handler", CallConvs = [typeof(CallConvCdecl)])]
    public static return_value_ptr* CreateHandler(param_ptr* p_owner,
        // PluginDelegates
        delegate* unmanaged[Cdecl]<param_ptr*, param_bool, return_value_json*> p_plugins_get_all,
        // ContextDelegates
        delegate* unmanaged[Cdecl]<param_ptr*, return_value_string*> p_context_get_app_version,
        delegate* unmanaged[Cdecl]<param_ptr*, return_value_string*> p_context_get_current_game_version,
        delegate* unmanaged[Cdecl]<param_ptr*, param_string*, return_value_string*> p_context_get_extender_version,
        // UI Delegates
        delegate* unmanaged[Cdecl]<param_ptr*, param_string*, param_json*, param_ptr*, delegate* unmanaged[Cdecl]<param_ptr*, param_int, param_int, param_json*, return_value_void*, void>, delegate* unmanaged[Cdecl]<param_ptr*, param_bool, param_int, return_value_void*, void>, delegate* unmanaged[Cdecl]<param_ptr*, return_value_void*, void>, return_value_void*> p_ui_start_dialog,
        delegate* unmanaged[Cdecl]<param_ptr*, return_value_void*> p_ui_end_dialog,
        delegate* unmanaged[Cdecl]<param_ptr*, param_json*, param_int, return_value_void*> p_ui_update_state
        )
    {
#if DEBUG
        using var logger = LogMethod();
#else
        using var logger = LogMethod();
#endif
        
        try
        {
            var pluginsDelegate = new CallbackPluginDelegates(p_owner,
                Marshal.GetDelegateForFunctionPointer<N_Plugins_GetAll>(new IntPtr(p_plugins_get_all))
            );

            var iniDelegate = new CallbackIniDelegates(p_owner,
                null!,
                null!
            );

            var contextDelegate = new CallbackContextDelegates(p_owner,
                Marshal.GetDelegateForFunctionPointer<N_Context_GetAppVersion>(new IntPtr(p_context_get_app_version)),
                Marshal.GetDelegateForFunctionPointer<N_Context_GetCurrentGameVersion>(new IntPtr(p_context_get_current_game_version)),
                Marshal.GetDelegateForFunctionPointer<N_Context_GetExtenderVersion>(new IntPtr(p_context_get_extender_version)),
                null!,
                null!,
                null!,
                null!
            );

            var uiDelegate = new CallbackUIDelegates(p_owner,
                Marshal.GetDelegateForFunctionPointer<N_UI_StartDialog>(new IntPtr(p_ui_start_dialog)),
                Marshal.GetDelegateForFunctionPointer<N_UI_EndDialog>(new IntPtr(p_ui_end_dialog)),
                Marshal.GetDelegateForFunctionPointer<N_UI_UpdateState>(new IntPtr(p_ui_update_state)),
                null!
            );

            var coreDelegates = new NativeCoreDelegatesHandler(p_owner, pluginsDelegate, contextDelegate, iniDelegate, uiDelegate);

            return return_value_ptr.AsValue(coreDelegates.HandlePtr, false);
        }
        catch (Exception e)
        {
            logger.LogException(e);
            return return_value_ptr.AsException(e, false);
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "dispose_handler", CallConvs = [typeof(CallConvCdecl)])]
    public static return_value_void* DisposeHandler(param_ptr* p_handle)
    {
#if DEBUG
        using var logger = LogMethod();
#else
        using var logger = LogMethod();
#endif
        
        try
        {
            if (p_handle is null || NativeCoreDelegatesHandler.FromPointer(p_handle) is not { } handler)
                return return_value_void.AsError(BUTR.NativeAOT.Shared.Utils.Copy("Handler is null or wrong!", false), false);

            handler.Dispose();

            return return_value_void.AsValue(false);
        }
        catch (Exception e)
        {
            logger.LogException(e);
            return return_value_void.AsException(e, false);
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "test_supported", CallConvs = [typeof(CallConvCdecl)]), IsNotConst<IsPtrConst>]
    public static return_value_json* TestSupported(
        // param_ptr* p_handle,
        [IsConst<IsPtrConst>] param_json* p_mod_archive_file_list,
        [IsConst<IsPtrConst>] param_json* p_allowed_types)
    {
#if DEBUG
        using var logger = LogMethod(p_mod_archive_file_list, p_allowed_types);
#else
        using var logger = LogMethod();
#endif
        
        try
        {
            //if (p_handle is null || NativeCoreDelegatesHandler.FromPointer(p_handle) is not { } handler)
            //    return return_value_json.AsError(BUTR.NativeAOT.Shared.Utils.Copy("Handler is null or wrong!", false), false);

            var modArchiveFileList = BUTR.NativeAOT.Shared.Utils.DeserializeJson(p_mod_archive_file_list, CustomSourceGenerationContext.StringArray);
            var allowedTypes = BUTR.NativeAOT.Shared.Utils.DeserializeJson(p_allowed_types, CustomSourceGenerationContext.StringArray);

            var result = Installer.TestSupported(modArchiveFileList.ToList(), allowedTypes.ToList());

            return return_value_json.AsValue(result, CustomSourceGenerationContext.SupportedResult, false);
        }
        catch (Exception e)
        {
            logger.LogException(e);
            return return_value_json.AsException(e, false);
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "cancel_handler", CallConvs = [typeof(CallConvCdecl)])]
    public static return_value_void* CancelHandler(param_ptr* handle)
    {
        try { NativeCoreDelegatesHandler.FromPointer(handle)!.RequestClose(); return return_value_void.AsValue(false); }
        catch (Exception e) { return return_value_void.AsException(e, false); }
    }
    [UnmanagedCallersOnly(EntryPoint = "handler_is_idle", CallConvs = [typeof(CallConvCdecl)])]
    public static return_value_bool* HandlerIsIdle(param_ptr* handle)
    {
        try { return return_value_bool.AsValue(NativeCoreDelegatesHandler.FromPointer(handle)!.IsIdle, false); }
        catch (Exception e) { return return_value_bool.AsException(e, false); }
    }
    [UnmanagedCallersOnly(EntryPoint = "install", CallConvs = [typeof(CallConvCdecl)]), IsNotConst<IsPtrConst>]
    public static return_value_async* Install(param_ptr* handle,
        [IsConst<IsPtrConst>] param_json* files, [IsConst<IsPtrConst>] param_json* patterns,
        [IsConst<IsPtrConst>] param_string* plugin, [IsConst<IsPtrConst>] param_string* script,
        [IsConst<IsPtrConst>] param_json* p_preset, [IsConst<IsPtrConst>] param_bool preselect,
        [IsConst<IsPtrConst>] param_bool validate, param_ptr* callbackOwner,
        delegate* unmanaged[Cdecl]<param_ptr*, return_value_json*, void> callback)
    {
        NativeCoreDelegatesHandler? handler = null;
        bool accepted = false;
        try
        {
            handler = NativeCoreDelegatesHandler.FromPointer(handle) ?? throw new ArgumentException("Invalid handler");
            var list = BUTR.NativeAOT.Shared.Utils.DeserializeJson(files, CustomSourceGenerationContext.StringArray);
            var stops = BUTR.NativeAOT.Shared.Utils.DeserializeJson(patterns, CustomSourceGenerationContext.StringArray);
            var pluginPath = plugin is null ? null : new string(param_string.ToSpan(plugin));
            var scriptPath = new string(param_string.ToSpan(script));
            var preset = BUTR.NativeAOT.Shared.Utils.DeserializeJson(p_preset, CustomSourceGenerationContext.JsonDocument);
            if (!handler.TryBeginInstall()) throw new InvalidOperationException("Installer is closing or already active");
            accepted = true;
            var completion = Installer.Install(list.ToList(), stops.ToList(), pluginPath, scriptPath, preset,
                preselect, validate, new ProgressDelegate(_ => { }), handler).ContinueWith(task =>
            {
                return_value_json* payload;
                try
                {
                    payload = task.Exception is not null ? return_value_json.AsException(task.Exception, false)
                        : return_value_json.AsValue(task.IsCanceled ? null : task.Result, CustomSourceGenerationContext.InstallResult, false);
                }
                catch (Exception e) { payload = return_value_json.AsException(e, false); }
                // Exactly one native result callback. Count through its actual return.
                try { callback(callbackOwner, payload); }
                finally { handler.EndInstall(); }
            }, System.Threading.Tasks.TaskScheduler.Default);
            handler.TrackCompletion(completion);
            return return_value_async.AsValue(false);
        }
        catch (Exception e)
        {
            if (accepted) handler!.AbortInstall();
            return return_value_async.AsException(e, false);
        }
    }
}
