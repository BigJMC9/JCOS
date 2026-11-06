<#
.SYNOPSIS
Record JA OS COM1 bytes on a Windows host and optionally send keyboard input.
.EXAMPLE
powershell.exe -NoProfile -File tools/capture_serial.ps1 -ListPorts
.EXAMPLE
powershell.exe -NoProfile -File tools/capture_serial.ps1 -Port COM3 -OutputPath build/rx570.log
.EXAMPLE
powershell.exe -NoProfile -File tools/capture_serial.ps1 -TcpHost 127.0.0.1 -TcpPort 4444 -OutputPath build/qemu.log
#>
[CmdletBinding(DefaultParameterSetName = 'Serial')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Serial')]
    [ValidateNotNullOrEmpty()][string]$Port,
    [Parameter(Mandatory = $true, ParameterSetName = 'Tcp')]
    [ValidateNotNullOrEmpty()][string]$TcpHost,
    [Parameter(Mandatory = $true, ParameterSetName = 'Tcp')]
    [ValidateRange(1, 65535)][int]$TcpPort,
    [Parameter(Mandatory = $true, ParameterSetName = 'List')]
    [switch]$ListPorts,
    [string]$OutputPath,
    [ValidateRange(0, 86400)][int]$DurationSeconds = 0,
    [switch]$NoInput,
    [switch]$Quiet,
    [string[]]$SendLine = @()
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($ListPorts) {
    [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
    return
}
if (-not $OutputPath) {
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
    $OutputPath = "build/serial-$stamp-$PID.log"
}
$resolvedOutput = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputPath)
[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($resolvedOutput))
$rawFile = $null
$metaFile = $null
$serialPort = $null
$tcpClient = $null
$stream = $null
$received = [long]0
$sent = [long]0
$started = [DateTime]::UtcNow
$reason = 'error or interruption'
$clock = [Diagnostics.Stopwatch]::StartNew()

function Send-Bytes([byte[]]$Bytes) {
    if ($Bytes.Length -eq 0) { return }
    if ($null -ne $script:serialPort) {
        $script:serialPort.Write($Bytes, 0, $Bytes.Length)
    } else {
        $script:stream.Write($Bytes, 0, $Bytes.Length)
    }
    $script:sent += $Bytes.Length
}

try {
    # Refuse replacement of either a previous transcript or its metadata.
    $rawFile = [IO.File]::Open($resolvedOutput, [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write, [IO.FileShare]::Read)
    $metaFile = [IO.File]::Open("$resolvedOutput.json", [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write, [IO.FileShare]::Read)
    if ($PSCmdlet.ParameterSetName -eq 'Serial') {
        $serialPort = New-Object System.IO.Ports.SerialPort
        $serialPort.PortName = $Port
        $serialPort.BaudRate = 38400
        $serialPort.DataBits = 8
        $serialPort.Parity = [IO.Ports.Parity]::None
        $serialPort.StopBits = [IO.Ports.StopBits]::One
        $serialPort.Handshake = [IO.Ports.Handshake]::None
        $serialPort.ReadTimeout = 100
        $serialPort.WriteTimeout = 1000
        $serialPort.ReadBufferSize = 65536
        $serialPort.DtrEnable = $true
        $serialPort.RtsEnable = $true
        $serialPort.Open()
        $endpoint = "$Port at 38400 8N1, no flow control"
    } else {
        $tcpClient = New-Object Net.Sockets.TcpClient
        $tcpClient.Connect($TcpHost, $TcpPort)
        $tcpClient.NoDelay = $true
        $stream = $tcpClient.GetStream()
        $stream.ReadTimeout = 100
        $stream.WriteTimeout = 1000
        $endpoint = $TcpHost + ":$TcpPort (QEMU TCP)"
    }
    $interactive = -not $NoInput -and -not [Console]::IsInputRedirected
    Write-Host "Capturing $endpoint to $resolvedOutput"
    Write-Host 'Start capture before boot. Received bytes are flushed after each chunk.'
    if ($interactive) { Write-Host 'Keyboard input enabled. Ctrl+] stops capture; Enter sends LF.' }
    else { Write-Host 'Input disabled. Ctrl+C stops capture.' }
    foreach ($line in $SendLine) {
        Send-Bytes ([Text.Encoding]::ASCII.GetBytes($line + "`n"))
    }
    $buffer = New-Object byte[] 4096
    $clock.Restart()
    while ($true) {
        if ($DurationSeconds -gt 0 -and $clock.Elapsed.TotalSeconds -ge $DurationSeconds) {
            $reason = 'duration elapsed'
            break
        }
        $count = -1
        try {
            if ($null -ne $serialPort) { $count = $serialPort.Read($buffer, 0, $buffer.Length) }
            else { $count = $stream.Read($buffer, 0, $buffer.Length) }
        } catch {
            $cause = $_.Exception.GetBaseException()
            $timeout = $cause -is [TimeoutException]
            if ($cause -is [Net.Sockets.SocketException]) {
                $timeout = $cause.SocketErrorCode -eq [Net.Sockets.SocketError]::TimedOut -or
                    $cause.SocketErrorCode -eq [Net.Sockets.SocketError]::WouldBlock
            }
            if (-not $timeout) { throw }
        }
        if ($count -eq 0 -and $null -ne $stream) {
            $reason = 'remote connection closed'
            break
        }
        if ($count -gt 0) {
            $rawFile.Write($buffer, 0, $count)
            $rawFile.Flush()
            $received += $count
            if (-not $Quiet) { [Console]::Write([Text.Encoding]::ASCII.GetString($buffer, 0, $count)) }
        }
        if ($interactive) {
            $stop = $false
            while ([Console]::KeyAvailable) {
                $key = [Console]::ReadKey($true)
                if ([int]$key.KeyChar -eq 29) { $stop = $true; break }
                $text = ''
                switch ($key.Key) {
                    'Enter'     { $text = "`n" }
                    'Backspace' { $text = [string][char]8 }
                    'Escape'    { $text = [string][char]27 }
                    'UpArrow'   { $text = [char]27 + '[A' }
                    'DownArrow' { $text = [char]27 + '[B' }
                    'RightArrow'{ $text = [char]27 + '[C' }
                    'LeftArrow' { $text = [char]27 + '[D' }
                    'Home'      { $text = [char]27 + '[H' }
                    'End'       { $text = [char]27 + '[F' }
                    'Delete'    { $text = [char]27 + '[3~' }
                    default {
                        if ([int]$key.KeyChar -gt 0 -and [int]$key.KeyChar -lt 128) {
                            $text = [string]$key.KeyChar
                        }
                    }
                }
                if ($text.Length -gt 0) { Send-Bytes ([Text.Encoding]::ASCII.GetBytes($text)) }
            }
            if ($stop) { $reason = 'local stop key'; break }
        }
    }
} finally {
    if ($null -ne $serialPort) { $serialPort.Dispose() }
    if ($null -ne $stream) { $stream.Dispose() }
    if ($null -ne $tcpClient) { $tcpClient.Close() }
    if ($null -ne $rawFile) { $rawFile.Dispose() }
    if ($null -ne $metaFile) {
        $metadata = [ordered]@{
            started_utc = $started.ToString('o')
            ended_utc = [DateTime]::UtcNow.ToString('o')
            transport = $PSCmdlet.ParameterSetName
            port = $Port
            tcp_host = $TcpHost
            tcp_port = $TcpPort
            baud = 38400
            framing = '8N1'
            flow_control = 'none'
            received_bytes = $received
            sent_bytes = $sent
            stop_reason = $reason
            raw_log = $resolvedOutput
            qualification = 'Capture alone does not establish physical GPU-failure recovery or independent serial control.'
        }
        $json = $metadata | ConvertTo-Json -Depth 3
        $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes($json + "`n")
        try {
            $metaFile.Write($bytes, 0, $bytes.Length)
            $metaFile.Flush()
        } finally { $metaFile.Dispose() }
    }
    Write-Host "`nCapture stopped: $received received bytes, $sent sent bytes."
}
