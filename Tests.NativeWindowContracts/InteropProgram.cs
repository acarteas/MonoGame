// MonoGame - Copyright (C) MonoGame Foundation, Inc
// This file is subject to the terms and conditions defined in
// file 'LICENSE.txt', which is part of this source code package.

using System;
using System.Reflection;
using System.Runtime.InteropServices;
using MonoGame.Interop;

internal static class InteropProgram
{
    private static void Check(bool condition)
    {
        if (!condition) { throw new Exception("Native drawable-size ABI contract failed."); }
    }

    private static unsafe void Main()
    {
        var method = typeof(MGP).GetMethod(nameof(MGP.Window_GetClientSize));
        var import = method.GetCustomAttribute<DllImportAttribute>();
        Check(import.Value == MGP.MonoGameNativeDLL);
        Check(import.EntryPoint == "MGP_Window_GetClientSize" && import.ExactSpelling);
        Check(import.CallingConvention == CallingConvention.Cdecl);
        Check(method.ReturnType == typeof(void));
        var parameters = method.GetParameters();
        Check(parameters.Length == 3 && parameters[0].ParameterType == typeof(MGP_Window*));
        Check(parameters[1].IsOut && parameters[1].ParameterType == typeof(int).MakeByRefType());
        Check(parameters[2].IsOut && parameters[2].ParameterType == typeof(int).MakeByRefType());
        Check(sizeof(int) == 4);
        Console.WriteLine("Actual Native platform interop drawable-size ABI metadata passed");
    }
}
