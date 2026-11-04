# MIT License; Copyright (c) 2026 Dominik Peters. See ../../LICENSE.
# Stdin lines contain deadline:base64 UTF-8 NUL-separated arguments, never code.
# Stdout lines contain exit-code:base64 UTF-8 output. EOF releases the process job.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
$jobSource = @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class TeXpressoLookupJob {
  [StructLayout(LayoutKind.Sequential)] struct Basic {
    public long ProcessTime, JobTime; public uint Flags;
    public UIntPtr MinWorking, MaxWorking; public uint ActiveProcesses;
    public UIntPtr Affinity; public uint Priority, Scheduling;
  }
  [StructLayout(LayoutKind.Sequential)] struct IO {
    public ulong ReadOps, WriteOps, OtherOps, ReadBytes, WriteBytes, OtherBytes;
  }
  [StructLayout(LayoutKind.Sequential)] struct Extended {
    public Basic Basic; public IO IO;
    public UIntPtr ProcessMemory, JobMemory, PeakProcessMemory, PeakJobMemory;
  }
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  static extern IntPtr CreateJobObject(IntPtr security, string name);
  [DllImport("kernel32.dll", SetLastError=true)]
  static extern bool SetInformationJobObject(IntPtr job, int type, ref Extended info, uint size);
  [DllImport("kernel32.dll", SetLastError=true)]
  static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
  [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
  static IntPtr ownedJob;
  public static volatile bool InputClosed;
  static System.Collections.Concurrent.ConcurrentQueue<string> requests = new System.Collections.Concurrent.ConcurrentQueue<string>();
  public static bool Next(out string request) { return requests.TryDequeue(out request); }
  static void WatchInput() {
    try { string line; while ((line = Console.In.ReadLine()) != null) requests.Enqueue(line); }
    catch {} finally { InputClosed = true; }
  }
  public static void Initialize() {
    ownedJob = CreateJobObject(IntPtr.Zero, null);
    var info = new Extended(); info.Basic.Flags = 0x2000; // KILL_ON_JOB_CLOSE
    if (ownedJob == IntPtr.Zero || !SetInformationJobObject(ownedJob,9,ref info,(uint)Marshal.SizeOf(info)) ||
        !AssignProcessToJobObject(ownedJob,GetCurrentProcess())) {
      int error = Marshal.GetLastWin32Error();
      if (ownedJob != IntPtr.Zero) CloseHandle(ownedJob);
      throw new Win32Exception(error);
    }
    var watcher = new System.Threading.Thread(WatchInput); watcher.IsBackground = true; watcher.Start();
    // Deliberately keep the non-inheritable handle until this process exits.
    // Its closure kills every child, even if a lookup/helper exits early.
  }
}
'@
function QuoteArgument([string] $value) {
  '"' + [regex]::Replace([regex]::Replace($value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}
try {
  $hash = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($jobSource))).Replace('-','')
  $cache = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'TeXpresso\bridge'
  $null = [IO.Directory]::CreateDirectory($cache)
  $assembly = Join-Path $cache ($hash + '.dll')
  $mutex = New-Object Threading.Mutex($false, ('Local\TeXpressoLookup-' + $hash))
  $locked = $false
  try {
    try { $locked = $mutex.WaitOne(30000) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (!$locked) { throw 'Timed out initializing the Windows lookup helper' }
    if (!(Test-Path -LiteralPath $assembly)) {
      $temporary = Join-Path $cache ([Guid]::NewGuid().ToString() + '.dll')
      try { Add-Type -TypeDefinition $jobSource -OutputAssembly $temporary; [IO.File]::Move($temporary,$assembly) }
      finally { if (Test-Path -LiteralPath $temporary) { [IO.File]::Delete($temporary) } }
    }
  } finally { if ($locked) { $mutex.ReleaseMutex() }; $mutex.Dispose() }
  Add-Type -Path $assembly
  [TeXpressoLookupJob]::Initialize()
  while (![TeXpressoLookupJob]::InputClosed) {
    $request = $null
    if (![TeXpressoLookupJob]::Next([ref]$request)) { Start-Sleep -Milliseconds 10; continue }
    $separator = $request.IndexOf(':')
    if ($separator -lt 1) { throw 'Invalid lookup request' }
    $limit = [long]::Parse($request.Substring(0,$separator))
    $encoded = $request.Substring($separator+1)
    $values = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($encoded)).Split([char]0)
    $bin = $values[0]; $tool = $values[1]
    if ($tool -notin @('findtexmf.exe','miktex.exe')) { throw 'Unsupported Windows MiKTeX tool' }
    if ($bin) { $executable = Join-Path $bin $tool }
    else {
      $command = Get-Command $tool -CommandType Application -ErrorAction SilentlyContinue
      if (!$command) {
        $env:PATH = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine')
        $command = Get-Command $tool -CommandType Application -ErrorAction Stop
      }
      $executable = $command.Source
    }
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $executable
    $info.Arguments = (($values | Select-Object -Skip 2 | ForEach-Object { QuoteArgument $_ }) -join ' ')
    $info.WorkingDirectory = [IO.Path]::GetTempPath()
    $info.UseShellExecute = $false; $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true; $info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = New-Object Text.UTF8Encoding($false)
    $info.StandardErrorEncoding = New-Object Text.UTF8Encoding($false)
    if ([TeXpressoLookupJob]::InputClosed) { exit 125 }
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info; $null = $process.Start(); $process.StandardInput.Close()
    $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while (!$process.WaitForExit(50)) {
      if ([TeXpressoLookupJob]::InputClosed -or $watch.ElapsedMilliseconds -ge $limit) { exit 124 }
    }
    # The job also owns descendants which retain redirected handles after exit.
    if (!$stdout.Wait(5000) -or !$stderr.Wait(5000)) { exit 124 }
    [Console]::Error.Write($stderr.Result)
    [Console]::Out.WriteLine([string]$process.ExitCode + ':' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($stdout.Result)))
    [Console]::Out.Flush()
  }
} catch { [Console]::Error.WriteLine('[miktex-windows] ' + $_.Exception.Message); exit 1 }
