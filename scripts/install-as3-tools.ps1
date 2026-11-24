param(
    [Parameter(Mandatory)]
    [string] $ApplicationDirectory
)

$ErrorActionPreference = 'Stop'
$application = (Resolve-Path -LiteralPath $ApplicationDirectory).ProviderPath
$directory = Join-Path $application 'tools/as3'
New-Item -ItemType Directory -Path $directory -Force | Out-Null

$tools = @(
    @{
        Name = 'asc.jar'
        Url = 'https://ftp.mozilla.org/pub/js/tamarin/builds/milestones/tamarin-central-01/asc.jar'
        Hash = 'EDDF909E25D557578E2D365B92889CAD1DB6A59E099E97F306C9FC3B48806D32'
    },
    @{
        Name = 'builtin.abc'
        Url = 'https://ftp.mozilla.org/pub/js/tamarin/builds/milestones/tamarin-central-01/builtin.abc'
        Hash = '90090BA78E0A8C13E04BC930CA4399DCFFA0653228119F69C6AC5544AA044D17'
    },
    @{
        Name = 'playerglobal.swc'
        Url = 'https://fpdownload.macromedia.com/get/flashplayer/updaters/32/playerglobal32_0.swc'
        Hash = '7D4D6168D27603CFB3B750302448E354E0BBC1BDD58F5D101C3DCF6891E9BB65'
    }
)

foreach ($tool in $tools) {
    $destination = Join-Path $directory $tool.Name
    if ((Test-Path -LiteralPath $destination) -and (Get-FileHash -LiteralPath $destination).Hash -eq $tool.Hash) {
        continue
    }
    $download = Join-Path $directory ([guid]::NewGuid().ToString() + '.download')
    try {
        Invoke-WebRequest -Uri $tool.Url -OutFile $download
        if ((Get-FileHash -LiteralPath $download).Hash -ne $tool.Hash) {
            throw "Unexpected checksum / Somme de contrôle inattendue ($($tool.Name))"
        }
        Move-Item -LiteralPath $download -Destination $destination -Force
    } finally {
        if (Test-Path -LiteralPath $download) {
            Remove-Item -LiteralPath $download
        }
    }
}
Write-Output $directory
