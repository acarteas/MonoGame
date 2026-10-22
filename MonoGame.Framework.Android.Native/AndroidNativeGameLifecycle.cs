using System;
using System.Text.Json.Serialization;

namespace Microsoft.Xna.Framework;

/// <summary>A point-in-time diagnostic from the Android Native game host.</summary>
public sealed class AndroidNativeGameLifecycle
{
    public ulong SessionId { get; internal set; }
    public string Stage { get; internal set; }
    public int UiThreadId { get; internal set; }
    public int GameThreadId { get; internal set; }
    public int UiNativeThreadId { get; internal set; }
    public int GameNativeThreadId { get; internal set; }
    public int? Result { get; internal set; }
    public string Failure { get; internal set; }
    public bool CancellationRequested { get; internal set; }
    public bool IsCallbackActive { get; internal set; }
    public bool IsRegistrationRooted { get; internal set; }
}

[JsonSerializable(typeof(AndroidNativeGameLifecycle))]
[JsonSourceGenerationOptions(GenerationMode = JsonSourceGenerationMode.Serialization)]
internal partial class AndroidNativeLifecycleJsonContext : JsonSerializerContext { }
