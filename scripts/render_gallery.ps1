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

function Format-Bytes([uint64]$bytes) {
    # Binary units, because that is what a card's memory is sold and reported
    # in, and one decimal, because the interesting comparison between two scenes
    # is never finer than that.
    if ($bytes -ge 1GB) { return ('{0:F1} GiB' -f ($bytes / 1GB)) }
    if ($bytes -ge 1MB) { return ('{0:F1} MiB' -f ($bytes / 1MB)) }
    if ($bytes -ge 1KB) { return ('{0:F1} KiB' -f ($bytes / 1KB)) }
    return ("$bytes B")
}

function Write-SceneStats($item, $stages, $seconds, $device, $settings) {
    # A versioned record of what a scene costs, beside the scene itself.
    #
    # Named for the .usda it describes, so the two travel together and a diff
    # says what changed about a scene rather than what changed about a table.
    # Two groups, because they answer different questions: the first is what the
    # scene *is* and changes only when the asset or the renderer's handling of
    # it does, and the second is what it cost on one machine on one day and
    # moves a little every run. A reviewer reading a diff wants to know which of
    # those they are looking at without counting lines.
    $path = Join-Path $galleryRoot ($item.Key + '.stats')

    # What the scene *is*, checked against what it was.
    #
    # These keys do not move unless the asset changed or the renderer's
    # handling of it did, so a change is either the point of the commit or a
    # defect -- and the two are told apart by a person, not here. Reported
    # rather than thrown: the image gate already fails a render that moved,
    # and a deliberate change that legitimately alters ray counts should not
    # have to fight the suite to land.
    #
    # It exists because the counts were already carrying a defect nobody saw.
    # The committed chess_board.stats recorded a tracedRays written by a run
    # that had the intermittent nondeterminism, and it sat in the file
    # unremarked because writing a number is not the same as reading it.
    if (Test-Path -LiteralPath $path) {
        $previous = @{}
        foreach ($line in (Get-Content -LiteralPath $path)) {
            if ($line -match '^([A-Za-z][A-Za-z0-9]*)\s+(\d+)$') {
                $previous[$matches[1]] = $matches[2]
            }
        }
        $moved = [Collections.Generic.List[string]]::new()
        foreach ($key in @('instances', 'triangles', 'meshesRefined',
                           'subdivideInputPoints', 'subdivideOutputPoints',
                           'materialsCompiled', 'texturesLoaded',
                           'textureBytes', 'cameraRays', 'tracedRays',
                           'shadowRays', 'rayHash', 'hitHash')) {
            if (-not $previous.ContainsKey($key)) { continue }
            if (-not $stages.ContainsKey($key)) { continue }
            $now = ([uint64]$stages[$key]).ToString()
            if ($now -ne $previous[$key]) {
                $moved.Add(('{0}: {1} -> {2}' -f $key, $previous[$key], $now))
            }
        }
        if ($moved.Count -gt 0) {
            Write-Warning (("$($item.Title): the scene group moved since the " +
                            'committed stats. Either this commit means it to, ' +
                            'or the renderer did not reproduce itself.'))
            foreach ($line in $moved) { Write-Warning "    $line" }
        }
    }
    $out = [Collections.Generic.List[string]]::new()
    $out.Add("# hdClaude render stats -- $($item.Key).usda")
    $out.Add('#')
    $out.Add('# Reported by the renderer through GetRenderStats(), not measured')
    $out.Add('# from outside: only it knows what it is holding, and only it knows')
    $out.Add('# the moment a frame holds all of it at once.')
    $out.Add('#')
    $out.Add('# Times are total work summed across Hydra''s worker threads, so a')
    $out.Add('# stage can exceed the frame''s wall time -- that is threads working')
    $out.Add('# at once, not an error.')
    $out.Add('#')
    $out.Add('# traceMs is the path tracing itself, summed over the progressive')
    $out.Add('# calls. outsideSeconds is the wall time that trace, ingest and')
    $out.Add('# publish do not claim: opening the stage, plugin discovery, the')
    $out.Add('# scene index, writing the EXR and')
    $out.Add('# tearing the device down, none of which the renderer can see.')
    $out.Add('#')
    $out.Add("# Regenerate with: render_gallery.bat -Scene $($item.Key)")
    $out.Add('')
    $out.Add('[scene]')
    $out.Add(('{0,-22}{1}' -f 'settings', $settings))
    foreach ($key in @('instances', 'triangles', 'meshesRefined',
                       'subdivideInputPoints', 'subdivideOutputPoints',
                       'materialsCompiled', 'texturesLoaded', 'textureBytes',
                       'cameraRays', 'tracedRays', 'shadowRays', 'rayHash', 'hitHash',
                       'deviceBytesPeak')) {
        if ($stages.ContainsKey($key)) {
            $out.Add(('{0,-22}{1}' -f $key, [uint64]$stages[$key]))
        }
    }
    $out.Add('')
    $out.Add('[cost]')
    $out.Add(('{0,-22}{1}' -f 'device', $device))
    $out.Add(('{0,-22}{1}' -f 'measured', (Get-Date -Format 'yyyy-MM-dd')))
    $out.Add(('{0,-22}{1:F3}' -f 'wallSeconds', $seconds))
    foreach ($key in @('traceMs', 'ingestMs', 'publishMs', 'subdivideMs',
                       'materialMs', 'textureMs')) {
        if ($stages.ContainsKey($key)) {
            $out.Add(('{0,-22}{1:F0}' -f $key, [double]$stages[$key]))
        }
    }

    # What the wall clock saw that no stage claims.
    #
    # Tracing, ingestion and publication are wall-clock stages of this
    # thread and can be subtracted from a wall time. Subdivision, material
    # and texture time cannot: they are summed across Hydra's workers and
    # happen *inside* ingest and publish, so counting them here would
    # subtract the same work twice. What is left is real work rather than
    # an error:
    # opening the stage, discovering plugins, populating the scene index,
    # writing the EXR, and tearing the device down. Recorded so that a
    # reader adding the stages up has somewhere to put the difference
    # instead of wondering what the machine was doing.
    if ($stages.ContainsKey('traceMs')) {
        $accounted = ([double]$stages['traceMs'] +
                      [double]$stages['ingestMs'] +
                      [double]$stages['publishMs']) / 1000.0
        $out.Add(('{0,-22}{1:F3}' -f 'outsideSeconds',
                  [Math]::Max(0.0, $seconds - $accounted)))
    }
    # Written without a byte-order mark. `Set-Content -Encoding utf8` on
    # Windows PowerShell writes one, and a committed text file that begins with
    # three invisible bytes is a file every other tool has to be told about.
    [System.IO.File]::WriteAllLines(
        $path, $out, (New-Object System.Text.UTF8Encoding $false))
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
    $lines.Add('| Scene | Measured | Wall time | Device memory | SHA-256 | Device | Settings |')
    $lines.Add('|---|---:|---:|---:|---|---|---|')
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
            # Width only; see the note where settingsText is built.
            $settings = "$($measurement.width) px wide, " +
                "$($measurement.samples) spp, $($measurement.samplesPerFrame)/update, " +
                "$($measurement.bounces) bounces, subdiv $($measurement.subdivision)"
            # A dash where the renderer did not say, which is every row measured
            # before it could. Zero would read as a scene that costs nothing.
            $peak = $null
            if ($measurement.stages -and
                $measurement.stages.PSObject.Properties.Name -contains 'deviceBytesPeak') {
                $peak = [uint64]$measurement.stages.deviceBytesPeak
            } elseif ($measurement.stages -is [hashtable] -and
                      $measurement.stages.ContainsKey('deviceBytesPeak')) {
                $peak = [uint64]$measurement.stages['deviceBytesPeak']
            }
            $memory = if ($null -ne $peak -and $peak -gt 0) {
                Format-Bytes $peak
            } else {
                '-'
            }
            $lines.Add("| $($item.Title) | $($measurement.date) | $exact s ($duration) | $memory | ``$hash`` | $($measurement.device) | $settings |")
        } else {
            $lines.Add("| $($item.Title) | - | Not measured | - | ``$hash`` | - | - |")
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

    # What the render costs in device memory, asked of the renderer rather than
    # measured from outside. Only it knows what it is holding, and only it knows
    # the moment a frame holds all of it -- path state, acceleration structures,
    # textures and film at once. It writes the figures to this file at teardown;
    # without the variable it writes nothing and says nothing.
    $reportPath = Join-Path $linearRoot "$($item.Key).stats.txt"
    if (Test-Path -LiteralPath $reportPath) {
        Remove-Item -LiteralPath $reportPath -Force
    }
    $env:HDCLAUDE_STATS_REPORT = $reportPath

    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    & $renderScript @arguments
    $renderExit = $LASTEXITCODE
    $stopwatch.Stop()
    Remove-Item Env:\HDCLAUDE_STATS_REPORT -ErrorAction SilentlyContinue
    if ($renderExit -ne 0) {
        throw "Render failed for $($item.Key) with exit code $renderExit"
    }
    if (!(Test-Path -LiteralPath $linearPath)) {
        throw "Render produced no image for $($item.Key): $linearPath"
    }

    # Absent rather than zero when the renderer said nothing. A missing figure
    # and a figure of nought are different facts, and a table that shows the
    # second for the first is a table that lies quietly.
    # Every key the renderer wrote, kept as it came. Reading them into a
    # dictionary rather than a fixed set of variables means a stage added on the
    # renderer's side appears in the JSON without this script being taught it.
    $stages = @{}
    if (Test-Path -LiteralPath $reportPath) {
        foreach ($line in Get-Content -LiteralPath $reportPath) {
            $parts = $line -split '\s+', 2
            if ($parts.Count -eq 2) {
                # Kept as text, not as a double. A 64-bit hash does not
                # survive one: hitHash 6942637114454757797 comes back as
                # ...757376, and two runs that genuinely disagreed could be
                # written to the committed stats as the same number. Every
                # use site says what it wants -- [uint64] for a count or a
                # hash, [double] where a fraction is meant.
                $stages[$parts[0]] = $parts[1].Trim()
            }
        }
    }
    $devicePeak = if ($stages.ContainsKey('deviceBytesPeak')) {
        [uint64]$stages['deviceBytesPeak']
    } else { $null }

    if ($stages.Count -gt 0) {
        Write-Host ((
            "  ingest:   {0:F0} ms snapshot, {1:F0} ms publish " +
            "({2:N0} instances, {3:N0} triangles)") -f
            [double]$stages['ingestMs'], [double]$stages['publishMs'],
            [uint64]$stages['instances'], [uint64]$stages['triangles'])
        Write-Host ("  subdiv:   {0:F0} ms over {1} meshes, {2:N0} -> {3:N0} points" -f
                    [double]$stages['subdivideMs'], [uint64]$stages['meshesRefined'],
                    [uint64]$stages['subdivideInputPoints'],
                    [uint64]$stages['subdivideOutputPoints'])
        Write-Host ((
            "  shading:  {0:F0} ms over {1} materials, " +
            "{2:F0} ms over {3} textures ({4})") -f
            [double]$stages['materialMs'], [uint64]$stages['materialsCompiled'],
            [double]$stages['textureMs'], [uint64]$stages['texturesLoaded'],
            (Format-Bytes ([uint64]$stages['textureBytes'])))
        Write-Host ("  trace:    {0:F0} ms tracing" -f
                    [double]$stages['traceMs'])
        Write-Host (("  rays:     {0:N0} from the camera, {1:N0} traced, " +
                     "{2:N0} shadow") -f
                    [uint64]$stages['cameraRays'], [uint64]$stages['tracedRays'],
                    [uint64]$stages['shadowRays'])
        if ($null -ne $devicePeak) {
            Write-Host ("  memory:   {0} on the device at the peak, {1} free" -f
                        (Format-Bytes $devicePeak),
                        (Format-Bytes ([uint64]$stages['deviceBytesAvailable'])))
        }
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
        # Whatever the renderer reported, verbatim. The table below shows one
        # of these; the rest are here because the interesting question about a
        # scene is usually not the one the table was built to answer.
        stages = $stages
        # Width only, because the height is the camera's to decide: these
        # scenes are framed by their own cameras and most are not square. The
        # table said "1024x1024" for an image 1024 by 434, which is the sort of
        # detail a reader takes on trust and should not have to.
        settingsText = "$imageWidth px wide, $samplesPerPixel spp, " +
            "$samplesPerFrame/update, $($env:HDCLAUDE_MAX_BOUNCES) bounces, " +
            "subdiv $($item.Subdivision)"
        date = Get-Date -Format 'yyyy-MM-dd'
        device = $device
        width = $imageWidth
        samples = $samplesPerPixel
        samplesPerFrame = $samplesPerFrame
        bounces = [int]$env:HDCLAUDE_MAX_BOUNCES
        subdivision = $item.Subdivision
    }

    if ($stages.Count -gt 0) {
        Write-SceneStats $item $stages $stopwatch.Elapsed.TotalSeconds `
            $device $timings[$item.Key].settingsText
    }

    # Written after every scene rather than at the end, so a run interrupted
    # half way still records what it measured.
    Write-Timings $timings
    Update-GalleryMarkdown $timings
    Write-Host ("Completed {0} in {1:F3} seconds" -f $item.Title,
                $stopwatch.Elapsed.TotalSeconds)
}
