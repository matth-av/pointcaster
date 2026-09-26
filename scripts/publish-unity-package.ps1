# Mirror the Unity package into an independent package repo
# that the Unity package manager can consume
#
#   ./scripts/publish-unity-package.ps1 -Remote git@github.com:matth-av/pointcaster-unity.git
#
# Authenticates with an ssh deploy key in UNITY_PACKAGE_DEPLOY_KEY.

param(
    [Parameter(Mandatory)] [string] $Remote,
    [string] $Branch = 'main',
    # Plugins-relative platform paths, e.g. Windows/x86_64
    [string[]] $Plugins = @('Windows/x86_64', 'Android/armeabi-v7a', 'Android/arm64-v8a'),
    [switch] $DryRun
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

$repo_root = Resolve-Path (Join-Path $PSScriptRoot '..')
$package = Join-Path $repo_root 'src/pointreceiver/unity'
$plugin_rels = $Plugins | ForEach-Object { "Plugins/$_" }
$version = (Get-Content (Join-Path $package 'package.json') -Raw | ConvertFrom-Json).version

# fail before touching the remote if any platform was not built and staged
foreach ($plugin_rel in $plugin_rels) {
    $staged = Get-ChildItem (Join-Path $package $plugin_rel) -File -ErrorAction SilentlyContinue |
        Where-Object Extension -in '.dll', '.so'
    if (-not $staged) {
        throw "no staged binaries in $plugin_rel - run stage-unity-package.ps1 for that platform first"
    }
}

$deploy_key = $env:UNITY_PACKAGE_DEPLOY_KEY
if (-not $deploy_key) { throw 'UNITY_PACKAGE_DEPLOY_KEY is not set' }

$work_root = Join-Path ([System.IO.Path]::GetTempPath()) "unity-package-$([guid]::NewGuid())"
New-Item -ItemType Directory -Path $work_root -Force | Out-Null

try {
    # ssh refuses a key file that other accounts can read
    $key_path = Join-Path $work_root 'deploy_key'
    [System.IO.File]::WriteAllText($key_path, ($deploy_key -replace "`r`n", "`n").TrimEnd() + "`n")
    $account = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
    icacls $key_path /inheritance:r /grant:r "${account}:(R)" | Out-Null
    $env:GIT_SSH_COMMAND = "ssh -i `"$key_path`" -o IdentitiesOnly=yes" +
        " -o StrictHostKeyChecking=accept-new" +
        " -o UserKnownHostsFile=`"$(Join-Path $work_root 'known_hosts')`""

    $work = Join-Path $work_root 'repo'
    New-Item -ItemType Directory -Path $work -Force | Out-Null
    git -C $work init --quiet --initial-branch $Branch
    git -C $work remote add origin $Remote
    if (git -C $work ls-remote --heads origin $Branch) {
        git -C $work fetch --quiet --depth 1 origin $Branch
        git -C $work checkout --quiet -B $Branch FETCH_HEAD
    }

    Get-ChildItem $work -Force |
        Where-Object Name -notin '.git', 'Plugins' |
        Remove-Item -Recurse -Force
    Get-ChildItem $package -Force |
        Where-Object Name -notin 'Plugins', '.gitignore' |
        Copy-Item -Destination $work -Recurse -Force

    foreach ($plugin_rel in $plugin_rels) {
        Remove-Item (Join-Path $work $plugin_rel) -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Path (Join-Path $work $plugin_rel) -Force | Out-Null
        Copy-Item (Join-Path $package "$plugin_rel/*") -Destination (Join-Path $work $plugin_rel) -Force

        # unity wants a .meta for every folder on the way down to the plugin
        $segments = $plugin_rel -split '/'
        for ($i = 0; $i -lt $segments.Count; $i++) {
            $meta = ($segments[0..$i] -join '/') + '.meta'
            if (Test-Path (Join-Path $package $meta)) {
                Copy-Item (Join-Path $package $meta) -Destination (Join-Path $work $meta) -Force
            }
        }
    }

    # android library projects are package source rather than build output,
    # so they ship on every publish no matter which platforms were staged
    $androidlibs = Get-ChildItem (Join-Path $package 'Plugins/Android') -Directory `
        -Filter '*.androidlib' -ErrorAction SilentlyContinue
    foreach ($androidlib in $androidlibs) {
        New-Item -ItemType Directory -Path (Join-Path $work 'Plugins/Android') -Force | Out-Null
        foreach ($meta in 'Plugins.meta', 'Plugins/Android.meta') {
            Copy-Item (Join-Path $package $meta) -Destination (Join-Path $work $meta) -Force
        }
        $dest = Join-Path $work "Plugins/Android/$($androidlib.Name)"
        Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue
        Copy-Item $androidlib.FullName -Destination $dest -Recurse -Force
        Copy-Item "$($androidlib.FullName).meta" -Destination "$dest.meta" -Force
    }

    git -C $work add --all
    if (-not (git -C $work status --porcelain)) {
        Write-Host '==> nothing to publish'
        return
    }
    if ($DryRun) {
        Write-Host '==> dry run, would commit:'
        git -C $work status --short
        return
    }

    $commit = git -C $repo_root rev-parse --short HEAD
    git -C $work -c user.name='pointcaster ci' -c user.email='ci@pointcaster.net' `
        commit --quiet -m "pointreceiver $($Plugins -join ' ') $version from $commit"
    git -C $work push --quiet origin $Branch
    Write-Host "==> pushed $Branch to $Remote"

    # bumping package.json is what creates a release and the tag is what a unity
    # project pins with #v<version>
    $tag = "v$version"
    if (git -C $work ls-remote --tags origin "refs/tags/$tag") {
        Write-Host "==> $tag is already published, bump package.json to cut a new release"
    } else {
        git -C $work tag $tag
        git -C $work push --quiet origin $tag
        Write-Host "==> tagged $tag"
    }
} finally {
    $env:GIT_SSH_COMMAND = $null
    Remove-Item $work_root -Recurse -Force -ErrorAction SilentlyContinue
}
