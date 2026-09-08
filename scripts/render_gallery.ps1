<#
.SYNOPSIS
Render the versioned gallery, gate each image against its committed baseline,
and record the timings in gallery.md.

.DESCRIPTION
One scene at a time: render scene-linear EXR, time it, convert to a display
JPEG, and compare that JPEG against the committed baseline before replacing it.
A scene that fails the image gate stops the run with its candidate left in the
build tree to look at.

The gate is the point. hdCodex checked the renderer's exit status, and a run
that lost the Vulkan device wrote a black image and exited zero, which was
accepted (docs/lessons-from-hdcodex.md R9). Exit status says nothing about an
image, so hdClaudeImageDiff does.

Timings cover the render only -- renderer startup, stage loading, MaterialX
generation and compilation, geometry processing, and sampling. Display
conversion and the gate are excluded, because they measure this script rather
than the renderer.

.PARAMETER Scene
Scene keys to render. Default: all of them, in table order.

.PARAMETER Accept
Adopt a changed image instead of failing on it. For a deliberate improvement;
the diff still runs and still prints what changed.

.PARAMETER UpdateOnly
Rewrite gallery.md from the recorded timings without rendering anything.

.EXAMPLE
render_gallery.bat
.EXAMPLE
render_gallery.bat -Scene chess_board, shader_ball_glass
.EXAMPLE
render_gallery.bat -Accept
#>
param(
    [string[]]$Scene,
    [switch]$Accept,
    [switch]$UpdateOnly
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$galleryRoot = Join-Path $projectRoot 'gallery'
$linearRoot = Join-Path $projectRoot 'build\gallery-linear'
$displayRoot = Join-Path $projectRoot 'build\gallery-display'
$timingPath = Join-Path $projectRoot 'build\gallery-timings.json'
$galleryMarkdown = Join-Path $projectRoot 'gallery.md'
$renderScript = Join-Path $projectRoot 'render_claude.bat'
$invariant = [Globalization.CultureInfo]::InvariantCulture

# The gallery contract: 1024 pixels wide, 1024 samples per pixel, 32 samples per
# progressive update. Fixed here rather than per scene so that every row of the
# table is comparable to every other, and to the same row a month from now.
$imageWidth = 1024
$samplesPerPixel = 1024
$samplesPerFrame = 32

# Scenes, in the order gallery.md lists them. The camera is part of the
# contract: usdrecord frames its own camera when a stage has none, and a framed
# camera moves whenever the geometry does, which would silently change what a
# baseline shows.
#
# UpAxis is the stage's own, and it is here because a Hydra scene delegate is
# never told it. It only affects a stage with no lights, where it aims the
# stand-in sun; a Y-up sun in the Z-up Kitchen Set shines along the floor.
$scenes = @(
    [pscustomobject]@{ Key = 'intel_sponza';          Title = 'Intel Sponza';                 Camera = 'PhysCamera001';     Purposes = $null;   Subdivision = 2; UpAxis = 'Y' },
    [pscustomobject]@{ Key = 'chess_board';           Title = 'OpenChessSet';                 Camera = 'renderCam';         Purposes = $null;   Subdivision = 2; UpAxis = 'Y' },
    [pscustomobject]@{ Key = 'shader_ball_gold';      Title = 'StandardShaderBall Gold';      Camera = 'camera';            Purposes = $null;   Subdivision = 2; UpAxis = 'Y' },
    [pscustomobject]@{ Key = 'shader_ball_glass';     Title = 'StandardShaderBall Glass';     Camera = 'camera';            Purposes = $null;   Subdivision = 2; UpAxis = 'Y' },
    [pscustomobject]@{ Key = 'shader_ball_bubblegum'; Title = 'StandardShaderBall BubbleGum'; Camera = 'camera';            Purposes = $null;   Subdivision = 2; UpAxis = 'Y' },
    [pscustomobject]@{ Key = 'shader_ball_honey';      Title = 'StandardShaderBall Honey';     Camera = 'camera';            Purposes = $null;   Subdivision = 2; UpAxis = 'Y' },
    [pscustomobject]@{ Key = 'pixar_kitchen';         Title = "Pixar's KitchenSet";           Camera = 'renderCam';         Purposes = $null;   Subdivision = 2; UpAxis = 'Z' },
    [pscustomobject]@{ Key = 'collectiveproject001';  Title = 'Collective Project 001';       Camera = 'mono';              Purposes = 'render'; Subdivision = 2; UpAxis = 'Y' },
    [pscustomobject]@{ Key = 'openpbr_playground';    Title = 'OpenPBR Playground';           Camera = 'renderCam_mainCU';  Purposes = 'render'; Subdivision = 2; UpAxis = 'Y' },
    [pscustomobject]@{ Key = 'subdivision_features';  Title = 'Subdivision Feature Matrix';   Camera = 'camera';            Purposes = $null;   Subdivision = 2; UpAxis = 'Y' },
    [pscustomobject]@{ Key = 'newzealand_heightmap';  Title = 'New Zealand Height Map';       Camera = 'camera';            Purposes = $null;   Subdivision = 6; UpAxis = 'Y' }
)

function Read-Timings {
    $result = [ordered]@{}
    if (!(Test-Path -LiteralPath $timingPath)) { return $result }
    $document = Get-Content -LiteralPath $timingPath -Raw | ConvertFrom-Json
    foreach ($property in $document.PSObject.Properties) {
        $result[$property.Name] = $property.Value
    }
    return $result
}

function Write-Timings($timings) {
    $json = $timings | ConvertTo-Json -Depth 4
    [IO.File]::WriteAllText($timingPath, $json, [Text.UTF8Encoding]::new($false))
}

function Format-Duration([double]$seconds) {
    $duration = [TimeSpan]::FromSeconds($seconds)
    if ($duration.TotalHours -ge 1.0) {
        return '{0}h {1}m {2:F3}s' -f [int][Math]::Floor($duration.TotalHours),
            $duration.Minutes, ($duration.Seconds + $duration.Milliseconds / 1000.0)
    }
    return '{0}m {1:F3}s' -f [int][Math]::Floor($duration.TotalMinutes),
        ($duration.Seconds + $duration.Milliseconds / 1000.0)
}

# The tools the gallery needs, from the install prefix the build writes to.
# Looked up after setup_usd_env.bat has run, because USDEXTRA is what it sets.
function Resolve-Tool([string]$name) {
    $candidates = @()
    if ($env:USDEXTRA) { $candidates += Join-Path $env:USDEXTRA "bin\$name.exe" }
    $candidates += Join-Path $projectRoot "build\dev\tools\$name.exe"
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw "$name was not found. Run compile.bat first; looked in: $($candidates -join ', ')"
}

# The GPU the timings belong to. A timing without the machine it was measured on
# cannot be compared, and a device-loss investigation cannot start from it
# (docs/lessons-from-hdcodex.md R11).
#
# Read from the GPU test binary, which prints the device it selected, rather
# than from a probe of its own: the selection logic is the renderer's, and a
# second implementation of it could report a device the renderer would not pick.
function Get-SelectedDevice {
    $probe = Join-Path $projectRoot 'build\dev\tests\hdClaudeGpuTests.exe'
    if (!(Test-Path -LiteralPath $probe)) { return 'Unknown' }
    $line = & $probe 2>$null | Where-Object { $_ -match '^\s*device\s+\.+\s+(.+)$' } |
        Select-Object -First 1
    if (!$line) { return 'Unknown' }
    return ($line -replace '^\s*device\s+\.+\s+', '').Trim()
}

function Update-GalleryMarkdown($timings) {
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add('<!-- gallery-timings:start -->')
    $lines.Add('| Scene | Measured | Wall time | SHA-256 | Device | Settings |')
    $lines.Add('|---|---:|---:|---|---|---|')
    foreach ($item in $scenes) {
        $baselinePath = Join-Path $galleryRoot ($item.Key + '.jpg')
        $hash = if (Test-Path -LiteralPath $baselinePath) {
            (Get-FileHash -LiteralPath $baselinePath -Algorithm SHA256).Hash.ToLowerInvariant()
        } else {
            '-'
        }
        if ($timings.Contains($item.Key)) {
            $measurement = $timings[$item.Key]
            $seconds = [double]$measurement.seconds
            $exact = $seconds.ToString('F3', $invariant)
            $duration = Format-Duration $seconds
            $settings = "$($measurement.width)x$($measurement.width), " +
                "$($measurement.samples) spp, $($measurement.samplesPerFrame)/update, " +
                "$($measurement.bounces) bounces, subdiv $($measurement.subdivision)"
            $lines.Add("| $($item.Title) | $($measurement.date) | $exact s ($duration) | ``$hash`` | $($measurement.device) | $settings |")
        } else {
            $lines.Add("| $($item.Title) | - | Not measured | ``$hash`` | - | - |")
        }
    }
    $lines.Add('<!-- gallery-timings:end -->')
    $table = $lines -join "`n"

    $source = [IO.File]::ReadAllText($galleryMarkdown)
    $pattern = '(?s)<!-- gallery-timings:start -->.*?<!-- gallery-timings:end -->'
    if (![Text.RegularExpressions.Regex]::IsMatch($source, $pattern)) {
        throw 'gallery.md has no generated timing-table markers'
    }
    $updated = [Text.RegularExpressions.Regex]::Replace($source, $pattern, $table)
    [IO.File]::WriteAllText($galleryMarkdown, $updated, [Text.UTF8Encoding]::new($false))
}

$timings = Read-Timings
if ($UpdateOnly) {
    Update-GalleryMarkdown $timings
    exit 0
}

$selected = $scenes
if ($Scene) {
    # Split on commas as well as on argument boundaries. render_gallery.bat
    # forwards its arguments to `powershell -File`, which hands an array
    # parameter through as one literal string, so `-Scene a,b` arrives here as
    # a single element and would otherwise be reported as an unknown scene.
    $keys = $Scene | ForEach-Object { $_ -split ',' } |
        ForEach-Object { $_.Trim() } | Where-Object { $_ }
    $requested = [Collections.Generic.HashSet[string]]::new(
        [string[]]$keys, [StringComparer]::OrdinalIgnoreCase)
    $selected = @($scenes | Where-Object { $requested.Contains($_.Key) })
    if ($selected.Count -ne $requested.Count) {
        throw "Unknown gallery scene. Known keys: $(($scenes.Key) -join ', ')"
    }
}

New-Item -ItemType Directory -Force -Path $linearRoot | Out-Null
New-Item -ItemType Directory -Force -Path $displayRoot | Out-Null

$displayTransform = Resolve-Tool 'hdClaudeDisplayTransform'
$imageDiff = Resolve-Tool 'hdClaudeImageDiff'
$device = Get-SelectedDevice

# Render settings, by the names the delegate actually reads
# (src/hydra/render_delegate.cpp). Bounces are left at the delegate's default of
# eight, which is what a transmissive material needs; it is recorded in the
# table so a later change to the default is visible rather than silent.
$env:HDCLAUDE_SAMPLES_PER_PIXEL = [string]$samplesPerPixel
$env:HDCLAUDE_SAMPLES_PER_FRAME = [string]$samplesPerFrame
if (!$env:HDCLAUDE_MAX_BOUNCES) { $env:HDCLAUDE_MAX_BOUNCES = '8' }
if (!$env:HDCLAUDE_GALLERY_EXPOSURE) { $env:HDCLAUDE_GALLERY_EXPOSURE = '0' }

foreach ($item in $selected) {
    $env:HDCLAUDE_SUBDIVISION_LEVEL = [string]$item.Subdivision
    $env:HDCLAUDE_UP_AXIS = [string]$item.UpAxis

    $scenePath = Join-Path $galleryRoot ($item.Key + '.usda')
    $linearPath = Join-Path $linearRoot ($item.Key + '.exr')
    $candidatePath = Join-Path $displayRoot ($item.Key + '.jpg')
    $baselinePath = Join-Path $galleryRoot ($item.Key + '.jpg')

    if (!(Test-Path -LiteralPath $scenePath)) {
        throw "Gallery stage is missing: $scenePath"
    }

    # The EXR stays scene-linear and the display transform below is the only
    # place a transfer function is applied. render_claude.bat enforces that by
    # passing --colorCorrectionMode disabled itself, so every caller gets it
    # rather than only this one.
    $arguments = @('--imageWidth', [string]$imageWidth)
    if ($item.Purposes) { $arguments += @('--purposes', $item.Purposes) }
    $arguments += @('--camera', $item.Camera, $scenePath, $linearPath)

    Write-Host "Rendering $($item.Title)..."
    if (Test-Path -LiteralPath $linearPath) { Remove-Item -LiteralPath $linearPath -Force }

    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    & $renderScript @arguments
    $renderExit = $LASTEXITCODE
    $stopwatch.Stop()
    if ($renderExit -ne 0) {
        throw "Render failed for $($item.Key) with exit code $renderExit"
    }
    if (!(Test-Path -LiteralPath $linearPath)) {
        throw "Render produced no image for $($item.Key): $linearPath"
    }

    # Scan the *linear* render, which is the actual rendered data, before
    # anything transforms it. The display transform below sanitises non-finite
    # values and clamps everything outside [0, 1], so the comparison that
    # follows -- which is a comparison of display images, and right to be -- can
    # see none of that. This is the only part of the gate that looks at what the
    # renderer actually produced.
    #
    # It was blind for as long as it existed. The OpenPBR Playground carried
    # values reaching 2.18e25 and three non-finite samples in its linear render,
    # clamped to white in the JPEG, and passed every time.
    & $imageDiff --scan $linearPath
    if ($LASTEXITCODE -ne 0 -and !$Accept) {
        throw ("$($item.Title) produced a linear render that fails its scan. " +
               "The image is at $linearPath.")
    }

    & $displayTransform $linearPath $candidatePath --exposure $env:HDCLAUDE_GALLERY_EXPOSURE
    if ($LASTEXITCODE -ne 0) {
        throw "Display conversion failed for $($item.Key)"
    }

    if (Test-Path -LiteralPath $baselinePath) {
        & $imageDiff $baselinePath $candidatePath
        $diffExit = $LASTEXITCODE
        if ($diffExit -ne 0 -and !$Accept) {
            throw ("$($item.Title) differs from its committed baseline. The " +
                   "candidate is at $candidatePath; rerun with -Accept to adopt it.")
        }
        if ($diffExit -ne 0) {
            Write-Host "  accepted a changed image for $($item.Title)"
        }
    } else {
        # Nothing to compare against yet. Say so: a first baseline is adopted
        # unexamined, and that is worth one line of output rather than silence.
        Write-Host "  no committed baseline yet; adopting this render as the first"
    }
    Copy-Item -LiteralPath $candidatePath -Destination $baselinePath -Force

    $timings[$item.Key] = [ordered]@{
        seconds = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
        date = Get-Date -Format 'yyyy-MM-dd'
        device = $device
        width = $imageWidth
        samples = $samplesPerPixel
        samplesPerFrame = $samplesPerFrame
        bounces = [int]$env:HDCLAUDE_MAX_BOUNCES
        subdivision = $item.Subdivision
    }

    # Written after every scene rather than at the end, so a run interrupted
    # half way still records what it measured.
    Write-Timings $timings
    Update-GalleryMarkdown $timings
    Write-Host ("Completed {0} in {1:F3} seconds" -f $item.Title,
                $stopwatch.Elapsed.TotalSeconds)
}
