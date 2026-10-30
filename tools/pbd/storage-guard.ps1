# Refuses a large job before it starts when the drive cannot hold it plus a reserve, so a full disk never interrupts a
# build or a dependency download halfway. The sizes are in storage-policy.json.
function Assert-PbdStorage([string]$Path, [string]$Job) {
    $policy = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'storage-policy.json') | ConvertFrom-Json
    $estimate = $policy.job_estimates_bytes.$Job
    if ($null -eq $estimate) { throw "Unknown storage job: $Job" }
    $absolute = [IO.Path]::GetFullPath($Path)
    $drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($absolute))
    $required = [long]$policy.reserve_bytes + [long]$estimate
    if ($drive.AvailableFreeSpace -lt $required) {
        $message = 'Not enough free space for {0}: {1:N2} GiB free, {2:N2} GiB needed including a {3:N0} GiB reserve.'
        $message = $message -f $Job, ($drive.AvailableFreeSpace / 1GB), ($required / 1GB), ($policy.reserve_bytes / 1GB)
        throw "$message Free some space or use another drive."
    }
}
