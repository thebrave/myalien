<#
.SYNOPSIS
    MyAlien Phase 0 - READ-ONLY hardware/firmware discovery for the Dell G15 5520.

.DESCRIPTION
    Dumps what the firmware exposes through Windows' inbox ACPI-WMI driver:
      * AWCC "WMAX" thermal interface (root\wmi : AWCCWmiMethodFunction)
      * Dell BIOS attributes (root\dcim\sysman\biosattributes) - battery / thermal related
      * Dell WMI security (root\dcim\sysman\wmisecurity) - is a BIOS password set?
      * Inbox battery & thermal-zone data
      * All ACPI-WMI GUID-backed classes in root\wmi (to find Dell event classes)
      * MyDell / FusionService state, Windows power scheme

    SAFETY: this script never writes anything to firmware. It only issues WMAX operations that the
    Linux alienware-wmi documentation describes as reads:
        Thermal_Information ops 0x02 0x03 0x04 0x05 0x06 0x08 0x09 0x0A 0x0B 0x0C
        GetFanSensors       ops 0x01 0x02
        GameShiftStatus     op  0x02 (get) - op 0x01 (toggle) is NEVER sent
    Thermal_Control, BIOSAttributeInterface methods and every other method are never invoked.

    Run elevated (admin) for the Dell sections; non-elevated runs still collect the inbox sections.

.PARAMETER Label
    Free text stored in the output, e.g. "mydell-running" or "fusion-stopped".

.PARAMETER OutDir
    Where to write the JSON dump. Default: <repo>\docs\discovery

.PARAMETER WatchEventsSeconds
    If > 0, also listen to Dell's BIOSEvent (root\wmi, GUID 9DBB5994-...) for that many seconds and record
    every event payload - press Fn shortcuts (especially Fn+F9 / F9 Game Shift G-key) and plug/unplug the adapter meanwhile. Read-only.

.EXAMPLE
    # From an elevated PowerShell:
    .\tools\discovery\Invoke-Discovery.ps1 -Label mydell-running -WatchEventsSeconds 30
#>
[CmdletBinding()]
param(
    [string]$Label = "default",
    [string]$OutDir,
    [int]$WatchEventsSeconds = 0
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

if (-not $OutDir) { $OutDir = Join-Path $PSScriptRoot '..\..\docs\discovery' }

function Hex([uint64]$v, [int]$w = 2) { '0x' + $v.ToString("X$w") }

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Warning "Not elevated: AWCC / Dell BIOS sections will fail with access denied. Re-run as admin for a full dump."
}

# Run a section, never let one failure abort the whole dump.
function Section([string]$name, [scriptblock]$body) {
    Write-Host "== $name" -ForegroundColor Cyan
    try { & $body }
    catch {
        Write-Host "   ERROR: $($_.Exception.Message)" -ForegroundColor Yellow
        [ordered]@{ error = $_.Exception.Message }
    }
}

# ---------------------------------------------------------------------------------------------
# WMAX helpers (read-only allow-list)
# ---------------------------------------------------------------------------------------------
$WmaxReadAllow = @{
    'Thermal_Information' = @(0x02, 0x03, 0x04, 0x05, 0x06, 0x08, 0x09, 0x0A, 0x0B, 0x0C)
    'GetFanSensors'       = @(0x01, 0x02)
    'GameShiftStatus'     = @(0x02)
}

$script:Wmax = $null

function Invoke-WmaxRead([string]$method, [int]$op, [int]$a1 = 0, [int]$a2 = 0, [int]$a3 = 0) {
    if (-not $WmaxReadAllow.ContainsKey($method) -or ($WmaxReadAllow[$method] -notcontains $op)) {
        throw "Refusing non-allow-listed WMAX call $method op $(Hex $op)"
    }
    $arg = [uint32](($op -band 0xFF) -bor (($a1 -band 0xFF) -shl 8) -bor (($a2 -band 0xFF) -shl 16) -bor (($a3 -band 0xFF) -shl 24))
    $r = Invoke-CimMethod -InputObject $script:Wmax -MethodName $method -Arguments @{ arg2 = $arg }
    $v = [uint32]$r.argr
    [ordered]@{
        arg2  = Hex $arg 8
        argr  = Hex $v 8
        value = $v
        error = ($v -eq [uint32]::MaxValue -or $v -eq ([uint32]::MaxValue - 1))
    }
}

$ProfileNames = @{
    0x00 = 'Custom'; 0xAB = 'G-Mode'
    0x96 = 'Quiet (legacy)'; 0x97 = 'Balanced (legacy)'; 0x98 = 'Balanced-Performance (legacy)'; 0x99 = 'Performance (legacy)'
    0xA0 = 'Balanced (USTT)'; 0xA1 = 'Balanced-Performance (USTT)'; 0xA2 = 'Cool (USTT)'
    0xA3 = 'Quiet (USTT)'; 0xA4 = 'Performance (USTT)'; 0xA5 = 'Low-Power (USTT)'
}
$FanNames = @{
    0x32 = 'CPU_1'; 0x33 = 'GPU_1'; 0x34 = 'PCI'; 0x35 = 'MID'; 0x36 = 'TOP_1'; 0x37 = 'SIDE'; 0x38 = 'U2_1'; 0x39 = 'U2_2'
    0x3A = 'FRONT_1'; 0x3B = 'CPU_2'; 0x3C = 'GPU_2'; 0x3D = 'TOP_2'; 0x3E = 'TOP_3'; 0x3F = 'FRONT_2'; 0x40 = 'BOTTOM_1'; 0x41 = 'BOTTOM_2'
}
$SensorNames = @{ 0x01 = 'CPU'; 0x03 = 'Front'; 0x06 = 'GPU' }

function NameOf($table, [int]$id) { if ($table.ContainsKey($id)) { $table[$id] } else { 'Unknown' } }

# ---------------------------------------------------------------------------------------------
$dump = [ordered]@{}

$dump.meta = Section 'Meta' {
    $cs = Get-CimInstance Win32_ComputerSystem
    $bios = Get-CimInstance Win32_BIOS
    $os = Get-CimInstance Win32_OperatingSystem
    [ordered]@{
        label        = $Label
        timestampUtc = (Get-Date).ToUniversalTime().ToString('o')
        elevated     = $isAdmin
        manufacturer = $cs.Manufacturer
        model        = $cs.Model
        systemSku    = $cs.SystemSKUNumber
        biosVersion  = $bios.SMBIOSBIOSVersion
        biosDate     = "$($bios.ReleaseDate)"
        os           = "$($os.Caption) $($os.Version) build $($os.BuildNumber)"
        psVersion    = "$($PSVersionTable.PSVersion)"
    }
}

$dump.mydell = Section 'MyDell / Fusion state' {
    $svc = Get-CimInstance Win32_Service | Where-Object { $_.PathName -match '\\Dell\\|Alienware' } |
        ForEach-Object { [ordered]@{ name = $_.Name; state = $_.State; startMode = $_.StartMode; path = $_.PathName } }
    $pkgs = Get-AppxPackage -ErrorAction SilentlyContinue | Where-Object { $_.Name -match 'Dell|Alienware' } |
        ForEach-Object { [ordered]@{ name = $_.Name; version = "$($_.Version)" } }
    [ordered]@{ services = @($svc); packages = @($pkgs) }
}

$dump.power = Section 'Windows power' {
    $overlay = $null
    try {
        Add-Type -Namespace MyAlien -Name PowrProf -MemberDefinition @'
[DllImport("powrprof.dll")] public static extern uint PowerGetEffectiveOverlayScheme(out Guid g);
'@ -ErrorAction SilentlyContinue
        $g = [Guid]::Empty
        if ([MyAlien.PowrProf]::PowerGetEffectiveOverlayScheme([ref]$g) -eq 0) { $overlay = "$g" }
    } catch { $overlay = "error: $($_.Exception.Message)" }
    [ordered]@{
        activeScheme         = ((powercfg /getactivescheme) -join ' ').Trim()
        effectiveOverlay     = $overlay
        overlayKnown         = @{
            '961cc777-2547-4f9d-8174-7d86181b8a7a' = 'Best power efficiency'
            '00000000-0000-0000-0000-000000000000' = 'Balanced'
            'ded574b5-45a0-4f42-8737-46345c09c238' = 'Best performance'
        }
        onAC                 = [bool](Get-CimInstance -Namespace root\wmi -ClassName BatteryStatus -ErrorAction SilentlyContinue |
                                      Select-Object -First 1 -ExpandProperty PowerOnline -ErrorAction SilentlyContinue)
    }
}

# ---------------------------------------------------------------------------------------------
# AWCC / WMAX
# ---------------------------------------------------------------------------------------------
$dump.awcc = Section 'AWCC WMAX interface (read-only)' {
    $cls = Get-CimClass -Namespace root\wmi -ClassName AWCCWmiMethodFunction
    $methods = $cls.CimClassMethods | ForEach-Object {
        $mid = $_.Qualifiers['WmiMethodId']
        [ordered]@{
            name        = $_.Name
            wmiMethodId = if ($mid) { [int]$mid.Value } else { $null }
            params      = @($_.Parameters | ForEach-Object { "$($_.CimType) $($_.Name)" })
        }
    }
    $guid = $cls.CimClassQualifiers['guid'].Value

    $instances = @(Get-CimInstance -Namespace root\wmi -ClassName AWCCWmiMethodFunction)
    $script:Wmax = $instances | Where-Object { $_.Active } | Select-Object -First 1
    if (-not $script:Wmax) { throw "No active AWCCWmiMethodFunction instance" }

    $out = [ordered]@{
        guid      = $guid
        instances = @($instances | ForEach-Object { [ordered]@{ instanceName = $_.InstanceName; active = $_.Active } })
        methods   = @($methods)
    }

    # System description
    $sd = Invoke-WmaxRead 'Thermal_Information' 0x02
    $v = $sd.value
    $counts = [ordered]@{
        fans     = [int]($v -band 0xFF)
        sensors  = [int](($v -shr 8) -band 0xFF)
        unknown  = [int](($v -shr 16) -band 0xFF)
        profiles = [int](($v -shr 24) -band 0xFF)
    }
    $out.systemDescription = [ordered]@{ raw = $sd; counts = $counts }
    foreach ($k in @($counts.Keys)) { if ($counts[$k] -gt 16) { throw "Implausible $k count $($counts[$k]) - stopping AWCC probe" } }

    # Resource IDs
    $total = $counts.fans + $counts.sensors + $counts.unknown + $counts.profiles
    $ids = @()
    for ($i = 0; $i -lt $total; $i++) {
        $r = Invoke-WmaxRead 'Thermal_Information' 0x03 $i
        $ids += [ordered]@{ index = $i; raw = $r.argr; id = [int]($r.value -band 0xFF); error = $r.error }
    }
    $out.resourceIds = $ids
    $fanIds     = @($ids | Select-Object -First $counts.fans)
    $sensorIds  = @($ids | Select-Object -Skip $counts.fans -First $counts.sensors)
    $unknownIds = @($ids | Select-Object -Skip ($counts.fans + $counts.sensors) -First $counts.unknown)
    $profIds    = @($ids | Select-Object -Skip ($counts.fans + $counts.sensors + $counts.unknown) -First $counts.profiles)

    $out.fans = @($fanIds | ForEach-Object {
        $id = $_.id
        $nTemps = Invoke-WmaxRead 'GetFanSensors' 0x01 $id
        $linked = @()
        if (-not $nTemps.error -and $nTemps.value -le 16) {
            for ($j = 0; $j -lt $nTemps.value; $j++) {
                $s = Invoke-WmaxRead 'GetFanSensors' 0x02 $id $j
                $linked += (Hex ($s.value -band 0xFF))
            }
        }
        [ordered]@{
            id             = Hex $id
            name           = NameOf $FanNames $id
            rpm            = Invoke-WmaxRead 'Thermal_Information' 0x05 $id
            minRpm         = Invoke-WmaxRead 'Thermal_Information' 0x08 $id
            maxRpm         = Invoke-WmaxRead 'Thermal_Information' 0x09 $id
            percent        = Invoke-WmaxRead 'Thermal_Information' 0x06 $id
            boost          = Invoke-WmaxRead 'Thermal_Information' 0x0C $id
            linkedSensorsN = $nTemps
            linkedSensors  = $linked
        }
    })

    $out.sensors = @($sensorIds | ForEach-Object {
        [ordered]@{
            id    = Hex $_.id
            name  = NameOf $SensorNames $_.id
            tempC = Invoke-WmaxRead 'Thermal_Information' 0x04 $_.id
        }
    })

    $out.unknownResources = @($unknownIds | ForEach-Object { Hex $_.id })

    $out.profiles = @($profIds | ForEach-Object {
        [ordered]@{ id = Hex $_.id; name = NameOf $ProfileNames $_.id; error = $_.error }
    })

    $cur = Invoke-WmaxRead 'Thermal_Information' 0x0B
    $out.currentProfile = [ordered]@{ raw = $cur; name = NameOf $ProfileNames ([int]($cur.value -band 0xFF)) }
    $out.op0A_balancedProfileId = Invoke-WmaxRead 'Thermal_Information' 0x0A
    $out.gameShiftStatus = Invoke-WmaxRead 'GameShiftStatus' 0x02

    # Short summary to console
    Write-Host ("   fans={0} sensors={1} unknown={2} profiles={3}" -f $counts.fans, $counts.sensors, $counts.unknown, $counts.profiles)
    foreach ($f in $out.fans) { Write-Host "   fan $($f.id) $($f.name): $($f.rpm.value) rpm (min $($f.minRpm.value) / max $($f.maxRpm.value)), boost $($f.boost.value)" }
    foreach ($s in $out.sensors) { Write-Host "   sensor $($s.id) $($s.name): $($s.tempC.value) C" }
    Write-Host "   profiles: $(($out.profiles | ForEach-Object { "$($_.id) $($_.name)" }) -join ', ')"
    Write-Host "   current: $($cur.argr) $($out.currentProfile.name); GameShift: $($out.gameShiftStatus.argr)"
    $out
}

# ---------------------------------------------------------------------------------------------
# Dell BIOS attributes (read only: instances + method signatures, nothing invoked)
# ---------------------------------------------------------------------------------------------
$dump.biosAttributes = Section 'Dell BIOS attributes' {
    $ns = 'root\dcim\sysman\biosattributes'
    $interesting = 'Batt|Charge|Therm|Fan|Power|Peak|Usb|AcBehavior|Wake|Cool|Perf|Turbo|Gmode|Game'
    $all = [ordered]@{}
    foreach ($cn in 'EnumerationAttribute', 'IntegerAttribute', 'StringAttribute') {
        $all[$cn] = @(Get-CimInstance -Namespace $ns -ClassName $cn | ForEach-Object {
            $o = [ordered]@{}
            foreach ($p in $_.CimInstanceProperties) {
                if ($p.Name -notin 'InstanceName', 'Active') { $o[$p.Name] = $p.Value }
            }
            # Machine identifiers (service tag / asset tag) are never written to the dump.
            if ($o['AttributeName'] -in 'SvcTag', 'Asset' -and $o.Contains('CurrentValue')) { $o['CurrentValue'] = '<redacted>' }
            $o
        })
    }
    $names = @($all.Values | ForEach-Object { $_ } | ForEach-Object { $_.AttributeName })
    $matching = @($all.Values | ForEach-Object { $_ } | Where-Object { $_.AttributeName -match $interesting })
    foreach ($m in $matching) {
        $pv = if ($m.Contains('PossibleValue')) { " [" + ($m.PossibleValue -join ',') + "]" }
              elseif ($m.Contains('LowerBound')) { " [$($m.LowerBound)..$($m.UpperBound) step $($m.ScalarIncrement)]" } else { '' }
        Write-Host "   $($m.AttributeName) = $($m.CurrentValue)$pv"
    }
    $iface = Get-CimClass -Namespace $ns -ClassName BIOSAttributeInterface
    $ifaceInstances = @(Get-CimInstance -Namespace $ns -ClassName BIOSAttributeInterface -ErrorAction SilentlyContinue |
        ForEach-Object { [ordered]@{ instanceName = $_.InstanceName; active = $_.Active } })
    [ordered]@{
        attributeCount     = $names.Count
        allAttributeNames  = $names
        interesting        = $matching
        all                = $all
        interfaceInstances = $ifaceInstances
        interfaceMethods   = @($iface.CimClassMethods | ForEach-Object {
            [ordered]@{
                name   = $_.Name
                params = @($_.Parameters | ForEach-Object {
                    $dir = if ($_.Qualifiers['In']) { 'in' } elseif ($_.Qualifiers['Out']) { 'out' } else { '?' }
                    "$dir $($_.CimType) $($_.Name)"
                })
            }
        })
    }
}

$dump.biosSecurity = Section 'Dell BIOS WMI security (password set?)' {
    $ns = 'root\dcim\sysman\wmisecurity'
    $classes = @(Get-CimClass -Namespace $ns | Where-Object { $_.CimClassName -notmatch '^(__|CIM_|MSFT_)' } |
        Select-Object -ExpandProperty CimClassName)
    $pw = $null
    if ($classes -contains 'PasswordObject') {
        $pw = @(Get-CimInstance -Namespace $ns -ClassName PasswordObject | ForEach-Object {
            [ordered]@{ name = $_.NameId; isPasswordSet = $_.IsPasswordSet }
        })
        foreach ($p in $pw) { Write-Host "   $($p.name): isPasswordSet=$($p.isPasswordSet)" }
    }
    [ordered]@{ classes = $classes; passwordObjects = $pw }
}

# ---------------------------------------------------------------------------------------------
# Inbox battery / thermal
# ---------------------------------------------------------------------------------------------
$dump.battery = Section 'Battery (inbox)' {
    function Props($o) {
        $h = [ordered]@{}
        foreach ($p in $o.CimInstanceProperties) {
            if ($null -ne $p.Value -and $p.Name -notmatch '^(Frequency_|Timestamp_)') { $h[$p.Name] = $p.Value }
        }
        $h
    }
    $r = [ordered]@{}
    foreach ($cn in 'BatteryStaticData', 'BatteryStatus', 'BatteryFullChargedCapacity', 'BatteryCycleCount',
                    'BatteryTemperature', 'BatteryRuntime') {
        try { $r[$cn] = @(Get-CimInstance -Namespace root\wmi -ClassName $cn | ForEach-Object { Props $_ }) }
        catch { $r[$cn] = "error: $($_.Exception.Message)" }
    }
    $r.Win32_Battery = @(Get-CimInstance Win32_Battery | ForEach-Object { Props $_ })
    try {
        Add-Type -Namespace MyAlien -Name BatteryIoctl -MemberDefinition @'
static Guid GUID_DEVCLASS_BATTERY = new Guid(0x72631e54, 0x78a4, 0x11d0, 0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a);
[StructLayout(LayoutKind.Sequential)] struct SP_DEVICE_INTERFACE_DATA { public int cbSize; public Guid InterfaceClassGuid; public int Flags; public IntPtr Reserved; }
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct SP_DEVICE_INTERFACE_DETAIL_DATA { public int cbSize; [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string DevicePath; }
[StructLayout(LayoutKind.Sequential)] struct BATTERY_QUERY_INFORMATION { public uint BatteryTag; public int InformationLevel; public int AtRate; }
[StructLayout(LayoutKind.Sequential)] public struct BATTERY_INFORMATION {
    public uint Capabilities; public byte Technology;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)] public byte[] Reserved;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)] public byte[] Chemistry;
    public uint DesignedCapacity; public uint FullChargedCapacity; public uint DefaultAlert1; public uint DefaultAlert2; public uint CriticalBias; public uint CycleCount;
}
[DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr SetupDiGetClassDevs(ref Guid ClassGuid, IntPtr Enumerator, IntPtr hwndParent, uint Flags);
[DllImport("setupapi.dll", SetLastError = true)] static extern bool SetupDiEnumDeviceInterfaces(IntPtr hDevInfo, IntPtr devInfo, ref Guid interfaceClassGuid, uint memberIndex, ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData);
[DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr hDevInfo, ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData, ref SP_DEVICE_INTERFACE_DETAIL_DATA deviceInterfaceDetailData, int deviceInterfaceDetailDataSize, out int requiredSize, IntPtr deviceInfoData);
[DllImport("setupapi.dll", SetLastError = true)] static extern bool SetupDiDestroyDeviceInfoList(IntPtr hDevInfo);
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern Microsoft.Win32.SafeHandles.SafeFileHandle CreateFile(string lpFileName, uint dwDesiredAccess, uint dwShareMode, IntPtr lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);
[DllImport("kernel32.dll", SetLastError = true)] static extern bool DeviceIoControl(Microsoft.Win32.SafeHandles.SafeFileHandle hDevice, uint dwIoControlCode, ref uint InBuffer, int nInBufferSize, out uint OutBuffer, int nOutBufferSize, out int pBytesReturned, IntPtr lpOverlapped);
[DllImport("kernel32.dll", SetLastError = true)] static extern bool DeviceIoControl(Microsoft.Win32.SafeHandles.SafeFileHandle hDevice, uint dwIoControlCode, ref BATTERY_QUERY_INFORMATION InBuffer, int nInBufferSize, out BATTERY_INFORMATION OutBuffer, int nOutBufferSize, out int pBytesReturned, IntPtr lpOverlapped);
public static BATTERY_INFORMATION QueryFirst(out string devPath) {
    devPath = null;
    IntPtr hDev = SetupDiGetClassDevs(ref GUID_DEVCLASS_BATTERY, IntPtr.Zero, IntPtr.Zero, 0x12);
    try {
        SP_DEVICE_INTERFACE_DATA did = new SP_DEVICE_INTERFACE_DATA(); did.cbSize = Marshal.SizeOf(did);
        if (!SetupDiEnumDeviceInterfaces(hDev, IntPtr.Zero, ref GUID_DEVCLASS_BATTERY, 0, ref did)) throw new System.ComponentModel.Win32Exception();
        SP_DEVICE_INTERFACE_DETAIL_DATA detail = new SP_DEVICE_INTERFACE_DETAIL_DATA(); detail.cbSize = IntPtr.Size == 8 ? 8 : 6;
        int req; SetupDiGetDeviceInterfaceDetail(hDev, ref did, ref detail, Marshal.SizeOf(detail), out req, IntPtr.Zero);
        devPath = detail.DevicePath;
        using (var hBatt = CreateFile(devPath, 0xC0000000u, 3, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
            uint wait = 0, tag = 0; int br;
            if (!DeviceIoControl(hBatt, 0x294040u, ref wait, 4, out tag, 4, out br, IntPtr.Zero)) throw new System.ComponentModel.Win32Exception();
            BATTERY_QUERY_INFORMATION bqi = new BATTERY_QUERY_INFORMATION { BatteryTag = tag, InformationLevel = 0 };
            BATTERY_INFORMATION bi;
            if (!DeviceIoControl(hBatt, 0x294044u, ref bqi, Marshal.SizeOf(bqi), out bi, Marshal.SizeOf(typeof(BATTERY_INFORMATION)), out br, IntPtr.Zero)) throw new System.ComponentModel.Win32Exception();
            return bi;
        }
    } finally { SetupDiDestroyDeviceInfoList(hDev); }
}
'@ -ErrorAction SilentlyContinue
        $devPath = $null
        $bi = [MyAlien.BatteryIoctl]::QueryFirst([ref]$devPath)
        $chem = ([System.Text.Encoding]::ASCII.GetString($bi.Chemistry)).Trim([char]0, ' ')
        $r.ioctlBatteryInformation = [ordered]@{
            devicePath          = $devPath
            designedCapacity    = $bi.DesignedCapacity
            fullChargedCapacity = $bi.FullChargedCapacity
            cycleCount          = $bi.CycleCount
            chemistry           = $chem
            technology          = $bi.Technology
            capabilities        = Hex $bi.Capabilities 8
        }
    } catch { $r.ioctlBatteryInformation = "error: $($_.Exception.Message)" }
    try {
        $design = if ($r.ioctlBatteryInformation -is [System.Collections.IDictionary]) { $r.ioctlBatteryInformation.designedCapacity }
                  else { ($r.BatteryStaticData | Select-Object -First 1).DesignedCapacity }
        $full = ($r.BatteryFullChargedCapacity | Select-Object -First 1).FullChargedCapacity
        if ($design) {
            $r.summary = [ordered]@{ designedmWh = $design; fullChargedmWh = $full; healthPercent = [math]::Round(100.0 * $full / $design, 1) }
            Write-Host "   design $design mWh, full $full mWh, health $($r.summary.healthPercent) %"
        }
    } catch { }
    $r
}

$dump.thermalZones = Section 'ACPI thermal zones (inbox)' {
    @(Get-CimInstance -Namespace root\wmi -ClassName MSAcpi_ThermalZoneTemperature | ForEach-Object {
        [ordered]@{ instance = $_.InstanceName; tempC = [math]::Round($_.CurrentTemperature / 10.0 - 273.15, 1); active = $_.Active }
    })
}

$dump.wmiGuidClasses = Section 'root\wmi ACPI-WMI GUID classes' {
    @(Get-CimClass -Namespace root\wmi | Where-Object { $_.CimClassQualifiers['guid'] } | ForEach-Object {
        [ordered]@{
            class      = $_.CimClassName
            superClass = $_.CimSuperClassName
            guid       = $_.CimClassQualifiers['guid'].Value
            isEvent    = ($_.CimSuperClassName -eq 'WMIEvent')
        }
    } | Sort-Object { $_.class })
}

# Dell-specific ACPI-WMI classes: definitions only (properties, methods, method IDs). Nothing is invoked.
# BFn       {A80593CE-...} = Dell SMBIOS calling interface (DoBFn -> SMI). Never invoked by MyAlien.
# WMI_Query {8D9DDCBC-...} = Dell WMI descriptor block.
# BIOSEvent {9DBB5994-...} = Dell hotkey / BIOS event (Linux dell-wmi).
$dump.dellWmiClassDefs = Section 'Dell WMI class definitions (BFn, WMI_Query, BIOSEvent)' {
    $r = [ordered]@{}
    foreach ($cn in 'BFn', 'WMI_Query', 'BIOSEvent') {
        try {
            $c = Get-CimClass -Namespace root\wmi -ClassName $cn
            $r[$cn] = [ordered]@{
                guid       = $c.CimClassQualifiers['guid'].Value
                properties = @($c.CimClassProperties | ForEach-Object { "$($_.CimType) $($_.Name)" })
                methods    = @($c.CimClassMethods | ForEach-Object {
                    $mid = $_.Qualifiers['WmiMethodId']
                    [ordered]@{
                        name        = $_.Name
                        wmiMethodId = if ($mid) { [int]$mid.Value } else { $null }
                        params      = @($_.Parameters | ForEach-Object {
                            $dir = if ($_.Qualifiers['In']) { 'in' } elseif ($_.Qualifiers['Out']) { 'out' } else { '?' }
                            "$dir $($_.CimType) $($_.Name)"
                        })
                    }
                })
            }
            Write-Host "   $cn $($r[$cn].guid): $(@($r[$cn].methods).Count) methods"
        } catch { $r[$cn] = "error: $($_.Exception.Message)" }
    }
    $r
}

# Optional: watch Dell BIOS events (Fn hotkeys / power-adapter events) for N seconds. Read-only.
if ($WatchEventsSeconds -gt 0) {
    $dump.biosEvents = Section "Watching root\wmi BIOSEvent for $WatchEventsSeconds s - press Fn+F9 (G-key) / Fn shortcuts / plug or unplug the adapter now" {
        $srcId = 'MyAlienBiosEvent'
        Register-CimIndicationEvent -Namespace root\wmi -ClassName BIOSEvent -SourceIdentifier $srcId | Out-Null
        $events = @()
        try {
            $deadline = (Get-Date).AddSeconds($WatchEventsSeconds)
            while ((Get-Date) -lt $deadline) {
                $e = Wait-Event -SourceIdentifier $srcId -Timeout 1
                if ($e) {
                    $ti = $e.SourceEventArgs.NewEvent
                    $o = [ordered]@{ timeUtc = $e.TimeGenerated.ToUniversalTime().ToString('o') }
                    # Payload is an embedded instance ("Data"); flatten one level, byte arrays as hex.
                    function Flat($inst, $prefix) {
                        foreach ($p in $inst.CimInstanceProperties) {
                            if ($p.Name -match '^(SECURITY_DESCRIPTOR|TIME_CREATED)$') { continue }
                            $v = $p.Value
                            if ($v -is [Microsoft.Management.Infrastructure.CimInstance]) { Flat $v "$prefix$($p.Name)." }
                            elseif ($v -is [array]) { $o["$prefix$($p.Name)"] = ($v | ForEach-Object { '{0:X2}' -f $_ }) -join ' ' }
                            else { $o["$prefix$($p.Name)"] = $v }
                        }
                    }
                    Flat $ti ''
                    Write-Host "   event: $(($o.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join '; ')"
                    $events += $o
                    Remove-Event -EventIdentifier $e.EventIdentifier
                }
            }
        } finally {
            Unregister-Event -SourceIdentifier $srcId -ErrorAction SilentlyContinue
        }
        [ordered]@{
            watchedSeconds = $WatchEventsSeconds
            count          = $events.Count
            events         = @($events)
        }
    }
}

# ---------------------------------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$safeLabel = ($Label -replace '[^\w\-]', '_')
$file = Join-Path $OutDir "discovery-$safeLabel-$stamp.json"
$dump | ConvertTo-Json -Depth 8 | Set-Content -Path $file -Encoding UTF8
Write-Host ""
Write-Host "Written: $((Resolve-Path $file).Path)" -ForegroundColor Green
