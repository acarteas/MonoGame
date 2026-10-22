using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Xml.Linq;
using System.Runtime.Serialization.Json;
using Microsoft.Build.Framework;
using Microsoft.Build.Utilities;

public sealed class ValidateAndroidVulkanRuntime : Task
{
    [Required]
    public string NativeDirectory { get; set; }
    [Required]
    public string ManifestPath { get; set; }
    [Required]
    public string Abi { get; set; }
    public string Configuration { get; set; }
    public string SourceRoot { get; set; }
    [Output]
    public ITaskItem[] Libraries { get; set; }

    static string Hash(byte[] bytes)
    {
        using (var hash = SHA256.Create())
        {
            return BitConverter.ToString(hash.ComputeHash(bytes)).Replace("-", "").ToLowerInvariant();
        }
    }
    static string HashFile(string path) => Hash(File.ReadAllBytes(path));
    static string Field(XElement value, string name) => value.GetProperty(name).GetString();
    static void Require(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
    static byte[] Git(string root, params string[] arguments)
    {
        var info = new ProcessStartInfo("git") { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
        var args = new List<string> { "-C", root };
        args.AddRange(arguments);
        info.Arguments = String.Join(" ", args.Select(arg => "\"" + arg.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\""));
        using (var process = Process.Start(info))
        using (var output = new MemoryStream())
        {
            var error = process.StandardError.ReadToEndAsync();
            process.StandardOutput.BaseStream.CopyTo(output);
            process.WaitForExit();
            Require(process.ExitCode == 0, "Cannot verify source checkout: " + error.GetAwaiter().GetResult());
            return output.ToArray();
        }
    }
    static void VerifySource(XElement source, string root)
    {
        Require(Directory.Exists(root), "Source checkout is missing: " + root);
        Require(Encoding.UTF8.GetString(Git(root, "rev-parse", "HEAD")).Trim() == Field(source, "commit"),
            "Native artifacts are stale: source HEAD changed at " + root + ". Re-run native/monogame/android/build.sh.");

        // Same byte framing and ordinal UTF-8 path ordering as build.sh's fingerprint.
        var names = Encoding.UTF8.GetString(Git(root, "ls-files", "--recurse-submodules", "-z"))
            .Split('\0').Where(name => name.Length != 0).OrderBy(name => name, StringComparer.Ordinal);
        using (var fingerprint = IncrementalHash.CreateHash(HashAlgorithmName.SHA256))
        {
            foreach (var name in names)
            {
                fingerprint.AppendData(Encoding.UTF8.GetBytes(name + "\0"));
                var path = Path.Combine(root, name);
                fingerprint.AppendData(File.Exists(path) ? File.ReadAllBytes(path) : Encoding.UTF8.GetBytes("<missing>"));
                fingerprint.AppendData(new byte[] { 0 });
            }
            Require(BitConverter.ToString(fingerprint.GetHashAndReset()).Replace("-", "").ToLowerInvariant() == Field(source, "trackedContentSha256"),
                "Native artifacts are stale: tracked source contents changed at " + root + ". Re-run native/monogame/android/build.sh.");
        }
    }
    public override bool Execute()
    {
        try
        {
            Require(Abi == "x86_64" || Abi == "arm64-v8a", "Unsupported Android Vulkan ABI '" + Abi + "'. Use android-x64 or android-arm64.");
            Require(File.Exists(ManifestPath), "Required Android Vulkan manifest is missing: " + ManifestPath + ". Build the requested ABI/configuration first.");
            using (var reader = JsonReaderWriterFactory.CreateJsonReader(File.ReadAllBytes(ManifestPath), System.Xml.XmlDictionaryReaderQuotas.Max))
            {
                var manifest = XDocument.Load(reader).Root;
                Require(manifest.GetProperty("schemaVersion").GetInt32() == 1 && Field(manifest, "phase") == "runtime" && Field(manifest, "result") == "ok",
                    "Manifest must describe a verified schema-1 Android runtime: " + ManifestPath);
                Require(Field(manifest, "abi") == Abi, "Manifest ABI mismatch: expected " + Abi + ": " + ManifestPath);
                var configuration = Field(manifest, "configuration");
                Require(configuration == "Debug" || configuration == "Release", "Invalid native configuration in " + ManifestPath);
                Require(String.IsNullOrEmpty(Configuration) || configuration == Configuration,
                    "Manifest configuration mismatch: expected " + Configuration + ": " + ManifestPath);
                Require(manifest.GetProperty("minApi").GetInt32() == 24 && manifest.GetProperty("compileSdk").GetInt32() == 36 &&
                    Field(manifest, "ndkRevision") == "30.0.16248370" && Field(manifest, "stl") == "c++_shared", "Manifest toolchain contract mismatch: " + ManifestPath);
                if (!String.IsNullOrEmpty(SourceRoot))
                {
                    var sources = manifest.GetProperty("sources");
                    VerifySource(sources.GetProperty("monoGame"), SourceRoot);
                    VerifySource(sources.GetProperty("sdl2Wrapper"), Path.Combine(SourceRoot, "native/monogame/external/sdl2"));
                    VerifySource(sources.GetProperty("sdl2"), Path.Combine(SourceRoot, "native/monogame/external/sdl2/sdl"));
                    VerifySource(sources.GetProperty("faudio"), Path.Combine(SourceRoot, "native/monogame/external/faudio"));
                    foreach (var input in manifest.GetProperty("buildInputs").Elements())
                    {
                        Require(HashFile(Path.Combine(SourceRoot, "native/monogame/android", input.Name.LocalName)) == Field(input, "sha256"), "Native build input is stale: " + input.Name.LocalName);
                    }
                }
                var result = new List<ITaskItem>();
                var names = new[] { "libmgruntime.so", "libSDL2.so", "libc++_shared.so" };
                var keys = new[] { "mgruntime", "sdl2", "cxxShared" };
                for (var i = 0; i < names.Length; i++)
                {
                    var path = Path.GetFullPath(Path.Combine(NativeDirectory, names[i]));
                    Require(File.Exists(path), "Required Android Vulkan artifact is missing: " + path + ". Build the requested ABI/configuration first.");
                    var bytes = File.ReadAllBytes(path);
                    var library = manifest.GetProperty("libraries").GetProperty(keys[i]);
                    Require(bytes.LongLength == library.GetProperty("bytes").GetInt64() && Hash(bytes) == Field(library, "sha256"), "Artifact hash/size mismatch (stale or corrupt): " + path);
                    Require(bytes.Length >= 64 && bytes[0] == 0x7f && bytes[1] == 0x45 && bytes[2] == 0x4c && bytes[3] == 0x46 && bytes[4] == 2 && bytes[5] == 1,
                        "Expected little-endian ELF64 Android library: " + path);
                    Require((bytes[18] | bytes[19] << 8) == (Abi == "x86_64" ? 62 : 183), "ELF ABI mismatch for " + Abi + ": " + path);
                    var item = new TaskItem(path);
                    item.SetMetadata("Abi", Abi);
                    item.SetMetadata("Link", "lib/" + Abi + "/" + names[i]);
                    item.SetMetadata("RuntimeIdentifier", Abi == "x86_64" ? "android-x64" : "android-arm64");
                    result.Add(item);
                }
                Libraries = result.ToArray();
            }
            return true;
        }
        catch (Exception error)
        {
            Log.LogError("Android Vulkan runtime validation failed: " + error.Message);
            return false;
        }
    }
}

internal static class AndroidVulkanJsonExtensions
{
    public static XElement GetProperty(this XElement value, string name)
    {
        var result = value.Element(name);
        if (result == null)
        {
            throw new InvalidOperationException("Manifest field is missing: " + name);
        }
        return result;
    }
    public static string GetString(this XElement value) => value.Value;
    public static int GetInt32(this XElement value) => Int32.Parse(value.Value, System.Globalization.CultureInfo.InvariantCulture);
    public static long GetInt64(this XElement value) => Int64.Parse(value.Value, System.Globalization.CultureInfo.InvariantCulture);
}
