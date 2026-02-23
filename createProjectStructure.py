#!/usr/bin/env python3
import argparse
import datetime as dt
import json
import re
import shutil
import shlex
import subprocess
import sys
import os
import urllib.request
from pathlib import Path

scriptVersion = "v1.0 (2026-02-23)"
defaultAwsServer = "admin@aandewiel.nl"
defaultAwsTarget = "/home/admin/flasherWebsite_v3"
defaultAwsSshKey = "~/.ssh/LightsailDefaultKey-eu-central-1.pem"
defaultProjectImageUrl = "https://flasher.aandewiel.nl/projects/ESP32project.png"

versionPattern = re.compile(r"v\d+\.\d+\.\d+")
envSectionPattern = re.compile(r"^\s*\[\s*env:([^\]]+)\s*\]\s*$")
semverPattern = re.compile(r"(\d+)\.(\d+)\.(\d+)")
versionWithPrefixPattern = re.compile(r"[vV](\d+\.\d+\.\d+)")
workspaceDirPattern = re.compile(r"^\s*workspace_dir\s*=\s*(.+?)\s*$", re.IGNORECASE)


def parsePlatformioSections(platformioIni: Path) -> dict[str, dict[str, str]]:
    sections: dict[str, dict[str, str]] = {}
    currentSection = None

    for rawLine in platformioIni.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = rawLine.strip()
        if not line or line.startswith(";") or line.startswith("#"):
            continue

        sectionMatch = re.match(r"^\[(.+)\]$", line)
        if sectionMatch:
            currentSection = sectionMatch.group(1).strip().lower()
            if currentSection not in sections:
                sections[currentSection] = {}
            continue

        if currentSection is None or "=" not in rawLine:
            continue

        key, value = rawLine.split("=", 1)
        normalizedKey = key.strip().lower()
        normalizedValue = re.split(r"\s[;#]", value, maxsplit=1)[0].strip()
        sections[currentSection][normalizedKey] = normalizedValue

    return sections


def getEnvConfigValue(
    sections: dict[str, dict[str, str]], envName: str, key: str
) -> str | None:
    normalizedKey = key.strip().lower()
    envSection = f"env:{envName}".lower()

    if envSection in sections and normalizedKey in sections[envSection]:
        return sections[envSection][normalizedKey]

    if "env" in sections and normalizedKey in sections["env"]:
        return sections["env"][normalizedKey]

    return None


def sanitizePathSegment(value: str) -> str:
    sanitized = re.sub(r"[^A-Za-z0-9._-]+", "_", value.strip())
    sanitized = sanitized.strip("._-")
    if not sanitized:
        return "unknown"
    return sanitized


def resolveEnvBoardName(sections: dict[str, dict[str, str]], envName: str) -> str:
    configuredBoard = getEnvConfigValue(sections, envName, "board")
    if configuredBoard:
        return sanitizePathSegment(configuredBoard)
    return sanitizePathSegment(envName)


def resolveEnvPartitionsSource(
    projectRoot: Path, sections: dict[str, dict[str, str]], envName: str
) -> Path | None:
    configuredValue = getEnvConfigValue(sections, envName, "board_build.partitions")

    candidates: list[Path] = []
    if configuredValue:
        cleaned = configuredValue.strip().strip('"').strip("'")
        cleaned = cleaned.replace("${PROJECT_DIR}", str(projectRoot))
        cleaned = cleaned.replace("$PROJECT_DIR", str(projectRoot))
        resolved = Path(cleaned).expanduser()
        if not resolved.is_absolute():
            resolved = (projectRoot / resolved).resolve()
        else:
            resolved = resolved.resolve()
        candidates.append(resolved)

    defaultCandidate = (projectRoot / "partitions.csv").resolve()
    candidates.append(defaultCandidate)

    for candidate in candidates:
        if candidate.exists() and candidate.is_file():
            return candidate

    return None


def parsePartitionsCsv(partitionsCsvPath: Path) -> dict[str, dict[str, str]]:
    partitions: dict[str, dict[str, str]] = {}

    for rawLine in partitionsCsvPath.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = rawLine.strip()
        if not line or line.startswith("#"):
            continue

        parts = [part.strip() for part in rawLine.split(",")]
        if len(parts) < 4:
            continue

        name = parts[0]
        if not name:
            continue

        partitions[name] = {
            "name": name,
            "type": parts[1] if len(parts) > 1 else "",
            "subtype": parts[2] if len(parts) > 2 else "",
            "offset": parts[3] if len(parts) > 3 else "",
            "size": parts[4] if len(parts) > 4 else "",
        }

    return partitions


def detectFirmwareOffset(partitions: dict[str, dict[str, str]]) -> str:
    if "factory" in partitions and partitions["factory"].get("offset"):
        return partitions["factory"]["offset"]
    if "app0" in partitions and partitions["app0"].get("offset"):
        return partitions["app0"]["offset"]

    for part in partitions.values():
        if part.get("type") == "app" and part.get("offset"):
            return part["offset"]

    return "0x10000"


def detectFilesystemOffset(partitions: dict[str, dict[str, str]]) -> str | None:
    directNames = ["spiffs", "littlefs", "fatfs"]
    for name in directNames:
        if name in partitions and partitions[name].get("offset"):
            return partitions[name]["offset"]

    for part in partitions.values():
        subtype = (part.get("subtype") or "").lower()
        if subtype in {"spiffs", "littlefs", "fatfs"} and part.get("offset"):
            return part["offset"]

    return None


def generateFlashJson(
    targetVersionDir: Path, envName: str, version: str, logLines: list[str]
) -> None:
    partitionsCsvPath = targetVersionDir / "partitions.csv"
    partitions: dict[str, dict[str, str]] = {}

    if partitionsCsvPath.exists():
        try:
            partitions = parsePartitionsCsv(partitionsCsvPath)
        except Exception as exc:
            logLines.append(f"WARN: partitions.csv parse failed: {exc}")

    flashFiles: list[dict[str, str]] = []

    bootloaderPath = targetVersionDir / "bootloader.bin"
    if bootloaderPath.exists():
        flashFiles.append({"offset": "0x1000", "file": "bootloader.bin"})

    partitionsBinPath = targetVersionDir / "partitions.bin"
    if partitionsBinPath.exists():
        flashFiles.append({"offset": "0x8000", "file": "partitions.bin"})

    bootAppPath = targetVersionDir / "boot_app0.bin"
    if bootAppPath.exists():
        flashFiles.append({"offset": "0xe000", "file": "boot_app0.bin"})

    firmwarePath = targetVersionDir / "firmware.bin"
    if firmwarePath.exists():
        firmwareOffset = detectFirmwareOffset(partitions)
        flashFiles.append({"offset": firmwareOffset, "file": "firmware.bin"})

    filesystemFile = None
    if (targetVersionDir / "LittleFS.bin").exists():
        filesystemFile = "LittleFS.bin"
    elif (targetVersionDir / "spiffs.bin").exists():
        filesystemFile = "spiffs.bin"

    if filesystemFile:
        filesystemOffset = detectFilesystemOffset(partitions)
        if filesystemOffset:
            flashFiles.append({"offset": filesystemOffset, "file": filesystemFile})
        else:
            logLines.append(
                f"WARN: No filesystem offset found in partitions.csv for {filesystemFile}"
            )

    flashPayload = {
        "board": envName,
        "version": version,
        "flash_files": flashFiles,
    }
    (targetVersionDir / "flash.json").write_text(
        json.dumps(flashPayload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


def parseEnvs(platformioIni: Path) -> list[str]:
    envs: list[str] = []
    for line in platformioIni.read_text(encoding="utf-8", errors="ignore").splitlines():
        match = envSectionPattern.match(line)
        if match:
            envs.append(match.group(1).strip())

    seen = set()
    uniqueEnvs = []
    for env in envs:
        if env and env not in seen:
            uniqueEnvs.append(env)
            seen.add(env)

    return uniqueEnvs


def getWorkspaceDir(platformioIni: Path, projectPath: Path) -> Path:
    sectionName = ""
    workspaceValue = None

    for rawLine in platformioIni.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = rawLine.strip()
        if not line or line.startswith(";") or line.startswith("#"):
            continue

        sectionMatch = re.match(r"^\[(.+)\]$", line)
        if sectionMatch:
            sectionName = sectionMatch.group(1).strip().lower()
            continue

        if sectionName != "platformio":
            continue

        keyMatch = workspaceDirPattern.match(rawLine)
        if keyMatch:
            workspaceValue = keyMatch.group(1).strip()
            break

    if not workspaceValue:
        return projectPath / ".pio"

    expanded = workspaceValue
    expanded = expanded.replace("${PROJECT_DIR}", str(projectPath))
    expanded = expanded.replace("$PROJECT_DIR", str(projectPath))
    expanded = expanded.replace("${platformio.packages_dir}", "")
    resolved = Path(expanded).expanduser()
    if not resolved.is_absolute():
        resolved = (projectPath / resolved).resolve()
    else:
        resolved = resolved.resolve()
    return resolved


def normalizeVersion(versionValue: str) -> str:
    match = semverPattern.search(versionValue)
    if not match:
        return "v0.0.0"
    return f"v{match.group(1)}.{match.group(2)}.{match.group(3)}"


def detectVersion(srcDir: Path) -> str:
    if not srcDir.is_dir():
        return "v0.0.0"

    for filePath in sorted(srcDir.rglob("*")):
        if not filePath.is_file():
            continue

        text = filePath.read_text(encoding="utf-8", errors="ignore")
        if "PROG_VERSION" not in text:
            continue

        for line in text.splitlines():
            if "PROG_VERSION" not in line:
                continue

            prefixedMatch = versionWithPrefixPattern.search(line)
            if prefixedMatch:
                return f"v{prefixedMatch.group(1)}"

            semverMatch = semverPattern.search(line)
            if semverMatch:
                return f"v{semverMatch.group(1)}.{semverMatch.group(2)}.{semverMatch.group(3)}"

            fallbackMatch = versionPattern.search(line)
            if fallbackMatch:
                return normalizeVersion(fallbackMatch.group(0))

    return "v0.0.0"


def runCommand(cmd: list[str], cwd: Path, logLines: list[str]) -> None:
    logLines.append(f"$ {' '.join(cmd)}")
    process = subprocess.run(cmd, cwd=str(cwd), text=True, capture_output=True)
    if process.stdout:
        logLines.append(process.stdout.rstrip())
    if process.stderr:
        logLines.append(process.stderr.rstrip())
    if process.returncode != 0:
        raise RuntimeError(
            f"Command failed ({process.returncode}): {' '.join(cmd)}\n{process.stderr}"
        )


def discoverBuildDir(projectRoot: Path, workspaceDir: Path, envName: str) -> Path:
    directCandidate = workspaceDir / "build" / envName
    if directCandidate.exists():
        return directCandidate

    buildRoot = workspaceDir / "build"
    if buildRoot.exists():
        for child in sorted(buildRoot.iterdir()):
            if not child.is_dir():
                continue
            if child.name == envName:
                return child
            if envName in child.name and (child / "firmware.bin").exists():
                return child

    fallbackPio = projectRoot / ".pio" / "build" / envName
    if fallbackPio.exists():
        return fallbackPio

    raise RuntimeError(
        f"Build directory not found for env '{envName}' in workspace_dir '{workspaceDir}' or fallback '.pio'."
    )


def ensureProjectMetaDataDefaults(rootDir: Path) -> Path:
    metaDataDir = rootDir / "projectMetaData"
    if metaDataDir.exists() and metaDataDir.is_dir():
        return metaDataDir

    metaDataDir.mkdir(parents=True, exist_ok=True)

    (metaDataDir / "project_en.md").write_text(
        "# your_project_name\n\nDiscription in English\n", encoding="utf-8"
    )
    (metaDataDir / "project_nl.md").write_text(
        "# your_project_name\n\nBeschrijving van het project in Dutch\n", encoding="utf-8"
    )

    payload = {
        "name": "your_project_name",
        "long_name_nl": "Langere naam in Dutch",
        "long_name_en": "longer name in English",
        "description_en": "Discription in English",
        "description_nl": "Beschrijving van het project in Dutch",
        "github_url": "https://github.com/mrWheel/",
        "post_url": "https://willem.aandewiel.nl/",
        "image": "thisProject.png",
    }
    (metaDataDir / "project.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )

    targetImage = metaDataDir / "thisProject.png"
    try:
        urllib.request.urlretrieve(defaultProjectImageUrl, str(targetImage))
    except Exception:
        targetImage.touch()

    return metaDataDir


def copyProjectMetaData(metaDataDir: Path, targetProjectDir: Path) -> None:
    for item in sorted(metaDataDir.iterdir()):
        if not item.is_file():
            continue

        destinationName = item.name
        if item.name == "ESP32project.png":
            destinationName = "thisProject.png"

        shutil.copy2(item, targetProjectDir / destinationName)


def copyIfExists(source: Path, destination: Path) -> bool:
    if source.exists() and source.is_file():
        shutil.copy2(source, destination)
        return True
    return False


def collectAndCopyArtifacts(
    projectRoot: Path,
    workspaceDir: Path,
    envName: str,
    targetVersionDir: Path,
    envPartitionsSource: Path | None,
    version: str,
    logLines: list[str],
) -> None:
    buildDir = discoverBuildDir(projectRoot, workspaceDir, envName)

    required = buildDir / "firmware.bin"
    if not required.exists():
        raise RuntimeError(f"firmware.bin not found for env '{envName}'")

    shutil.copy2(required, targetVersionDir / "firmware.bin")

    optionalFiles = [
        "boot_app0.bin",
        "bootloader.bin",
        "partitions.bin",
        "partitions.csv",
    ]
    for name in optionalFiles:
        copyIfExists(buildDir / name, targetVersionDir / name)

    targetPartitionsCsv = targetVersionDir / "partitions.csv"
    if envPartitionsSource and envPartitionsSource.exists():
        shutil.copy2(envPartitionsSource, targetPartitionsCsv)
        logLines.append(f"Using partitions source: {envPartitionsSource}")

    fsCandidates = [
        ("spiffs.bin", "spiffs.bin"),
        ("littlefs.bin", "LittleFS.bin"),
        ("LittleFS.bin", "LittleFS.bin"),
    ]
    for sourceName, destName in fsCandidates:
        if copyIfExists(buildDir / sourceName, targetVersionDir / destName):
            break

    generateFlashJson(targetVersionDir, envName, version, logLines)

    buildLogPath = targetVersionDir / "build_log.md"
    now = dt.datetime.now().isoformat(timespec="seconds")
    logBody = [f"# Build log for {envName}", "", f"Generated: {now}", ""]
    logBody.extend(logLines)
    logBody.append("")
    logBody.append(f"Resolved buildDir: {buildDir}")
    buildLogPath.write_text("\n".join(logBody).strip() + "\n", encoding="utf-8")


def resolveExecutable(commandName: str, preferredPaths: list[str]) -> str:
    for preferredPath in preferredPaths:
        pathObj = Path(preferredPath)
        if pathObj.exists() and os.access(pathObj, os.X_OK):
            return str(pathObj)

    detectedPath = shutil.which(commandName)
    if detectedPath:
        return detectedPath

    raise RuntimeError(f"Executable not found: {commandName}")


def syncProjectToAws(
    projectsRoot: Path,
    projectName: str,
    awsServer: str,
    awsTarget: str,
    awsSshKey: Path,
    awsDryRun: bool,
) -> None:
    rsyncPath = resolveExecutable("rsync", ["/usr/bin/rsync"])
    sshPath = resolveExecutable("ssh", ["/usr/bin/ssh"])

    sourceProjectPath = projectsRoot / projectName
    if not sourceProjectPath.exists():
        raise RuntimeError(f"Project directory does not exist for sync: {sourceProjectPath}")

    remoteProjectBase = f"{awsTarget.rstrip('/')}/projects"
    remoteProjectPath = f"{remoteProjectBase}/{projectName}"

    mkdirCmd = [
        sshPath,
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=10",
        "-i",
        str(awsSshKey),
        awsServer,
        f"mkdir -p {shlex.quote(remoteProjectPath)}",
    ]
    mkdirProcess = subprocess.run(mkdirCmd, text=True, capture_output=True)
    if mkdirProcess.returncode != 0:
        raise RuntimeError(
            f"Failed to create AWS project directory: {mkdirProcess.stderr.strip() or mkdirProcess.stdout.strip()}"
        )

    sshRsyncTransport = (
        f"{sshPath} -o BatchMode=yes -o ConnectTimeout=10 -i {shlex.quote(str(awsSshKey))}"
    )

    rsyncCmd = [
        rsyncPath,
        "-avz",
        "--update",
        "-e",
        sshRsyncTransport,
        "--exclude",
        ".DS_Store",
        "--exclude",
        "*.tmp",
        "--exclude",
        "*.bak",
        "--exclude",
        ".venv/",
    ]
    if awsDryRun:
        rsyncCmd.extend(["--dry-run", "--itemize-changes"])

    rsyncCmd.extend(
        [
            f"{sourceProjectPath}/",
            f"{awsServer}:{remoteProjectPath}/",
        ]
    )

    print("Starting AWS sync for project directory...")
    print(f"  Local:  {sourceProjectPath}")
    print(f"  Remote: {awsServer}:{remoteProjectPath}")
    print(f"  SSH key: {awsSshKey}")
    print(f"  Binaries: rsync={rsyncPath}, ssh={sshPath}")
    process = subprocess.run(rsyncCmd, text=True, capture_output=True)
    if process.stdout:
        print(process.stdout.strip())
    if process.stderr:
        print(process.stderr.strip())
    if process.returncode != 0:
        raise RuntimeError(f"AWS rsync failed with code {process.returncode}")


def syncProjectsFolderToAws(
    projectsRoot: Path,
    awsServer: str,
    awsTarget: str,
    awsSshKey: Path,
    awsDryRun: bool,
) -> None:
    rsyncPath = resolveExecutable("rsync", ["/usr/bin/rsync"])
    sshPath = resolveExecutable("ssh", ["/usr/bin/ssh"])

    if not projectsRoot.exists() or not projectsRoot.is_dir():
        raise RuntimeError(f"Projects directory does not exist for sync: {projectsRoot}")

    remoteProjectsPath = f"{awsTarget.rstrip('/')}/projects"

    mkdirCmd = [
        sshPath,
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=10",
        "-i",
        str(awsSshKey),
        awsServer,
        f"mkdir -p {shlex.quote(remoteProjectsPath)}",
    ]
    mkdirProcess = subprocess.run(mkdirCmd, text=True, capture_output=True)
    if mkdirProcess.returncode != 0:
        raise RuntimeError(
            f"Failed to create AWS projects directory: {mkdirProcess.stderr.strip() or mkdirProcess.stdout.strip()}"
        )

    sshRsyncTransport = (
        f"{sshPath} -o BatchMode=yes -o ConnectTimeout=10 -i {shlex.quote(str(awsSshKey))}"
    )

    rsyncCmd = [
        rsyncPath,
        "-avz",
        "--update",
        "-e",
        sshRsyncTransport,
        "--exclude",
        ".DS_Store",
        "--exclude",
        "*.tmp",
        "--exclude",
        "*.bak",
        "--exclude",
        ".venv/",
    ]
    if awsDryRun:
        rsyncCmd.extend(["--dry-run", "--itemize-changes"])

    rsyncCmd.extend(
        [
            f"{projectsRoot}/",
            f"{awsServer}:{remoteProjectsPath}/",
        ]
    )

    print("Starting AWS sync for full projects directory...")
    print(f"  Local:  {projectsRoot}")
    print(f"  Remote: {awsServer}:{remoteProjectsPath}")
    print(f"  SSH key: {awsSshKey}")
    print(f"  Binaries: rsync={rsyncPath}, ssh={sshPath}")
    process = subprocess.run(rsyncCmd, text=True, capture_output=True)
    if process.stdout:
        print(process.stdout.strip())
    if process.stderr:
        print(process.stderr.strip())
    if process.returncode != 0:
        raise RuntimeError(f"AWS rsync failed with code {process.returncode}")


def validateProjectsFolderForAwsSync(projectsRoot: Path) -> None:
    if not projectsRoot.exists() or not projectsRoot.is_dir():
        raise RuntimeError(f"Projects directory does not exist: {projectsRoot}")

    projectDirs = sorted([path for path in projectsRoot.iterdir() if path.is_dir()])
    if not projectDirs:
        raise RuntimeError(f"Projects directory is empty: {projectsRoot}")

    requiredMetaFiles = ["project.json", "project_en.md", "project_nl.md"]
    validationErrors: list[str] = []

    for projectDir in projectDirs:
        for fileName in requiredMetaFiles:
            if not (projectDir / fileName).is_file():
                validationErrors.append(
                    f"{projectDir.name}: missing metadata file '{fileName}'"
                )

        versionArtifacts = sorted(projectDir.rglob("flash.json"))
        if not versionArtifacts:
            validationErrors.append(
                f"{projectDir.name}: no build output found (flash.json is missing)"
            )
            continue

        hasValidArtifactSet = False
        for flashJsonPath in versionArtifacts:
            artifactDir = flashJsonPath.parent
            if (artifactDir / "firmware.bin").is_file():
                hasValidArtifactSet = True
                break

        if not hasValidArtifactSet:
            validationErrors.append(
                f"{projectDir.name}: invalid build output (firmware.bin is missing next to flash.json)"
            )

    if validationErrors:
        errorLines = "\n  - " + "\n  - ".join(validationErrors)
        raise RuntimeError(
            "Projects directory is not correctly populated for --only-sync-aws:" + errorLines
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        usage="%(prog)s [platformioProject] [--sync-aws | --only-sync-aws] [--aws-dry-run]",
        description=f"createProjectStructure.py {scriptVersion}\nCreate projects structure from a PlatformIO project.",
    )
    parser.add_argument(
        "platformioProject",
        nargs="?",
        help="Path to PlatformIO project",
    )
    syncModeGroup = parser.add_mutually_exclusive_group()
    syncModeGroup.add_argument(
        "--sync-aws",
        action="store_true",
        help="Sync generated projects/<project> directory to AWS (add/update only, never delete)",
    )
    syncModeGroup.add_argument(
        "--only-sync-aws",
        action="store_true",
        help="Skip build and only sync the full local projects directory to AWS",
    )
    parser.add_argument(
        "--aws-dry-run",
        action="store_true",
        help="Show AWS rsync changes without actually copying",
    )

    if len(sys.argv) == 1:
        parser.print_help()
        return 0

    args = parser.parse_args()

    if not args.platformioProject:
        parser.print_help()
        return 0

    projectPath = Path(args.platformioProject).expanduser().resolve()
    if not projectPath.exists() or not projectPath.is_dir():
        raise SystemExit(f"Invalid project path: {projectPath}")

    outputRoot = projectPath
    projectsRoot = outputRoot / "projects"

    if args.only_sync_aws:
        awsSshKey = Path(defaultAwsSshKey).expanduser().resolve()
        if not awsSshKey.exists():
            raise SystemExit(f"SSH key not found: {awsSshKey}")
        print(f"Validating projects directory: {projectsRoot}")
        validateProjectsFolderForAwsSync(projectsRoot)
        syncProjectsFolderToAws(
            projectsRoot=projectsRoot,
            awsServer=defaultAwsServer,
            awsTarget=defaultAwsTarget,
            awsSshKey=awsSshKey,
            awsDryRun=args.aws_dry_run,
        )
        print("Projects directory synchronized successfully.")
        return 0

    if shutil.which("pio") is None:
        raise SystemExit(
            "PlatformIO CLI not found. Install PlatformIO Core and ensure 'pio' is in PATH."
        )

    os.chdir(projectPath)

    platformioIni = projectPath / "platformio.ini"
    if not platformioIni.exists():
        raise SystemExit(f"platformio.ini not found in: {projectPath}")

    workspaceDir = getWorkspaceDir(platformioIni, projectPath)
    platformioSections = parsePlatformioSections(platformioIni)

    envs = parseEnvs(platformioIni)
    if not envs:
        raise SystemExit("No [env:...] sections found in platformio.ini")

    envBoardMap: dict[str, str] = {}
    boardCounts: dict[str, int] = {}
    for env in envs:
        boardName = resolveEnvBoardName(platformioSections, env)
        envBoardMap[env] = boardName
        boardCounts[boardName] = boardCounts.get(boardName, 0) + 1

    version = detectVersion(projectPath / "src")
    projectName = projectPath.name

    targetProjectDir = projectsRoot / projectName
    if targetProjectDir.exists():
        print(f"Removing existing project directory: {targetProjectDir}")
        shutil.rmtree(targetProjectDir)
    targetProjectDir.mkdir(parents=True, exist_ok=True)

    projectMetaDataDir = ensureProjectMetaDataDefaults(projectPath)
    copyProjectMetaData(projectMetaDataDir, targetProjectDir)

    print(f"createProjectStructure.py {scriptVersion}")
    print(f"Project: {projectName}")
    print(f"Version: {version}")
    print(f"Environments: {', '.join(envs)}")
    print("Boards per environment:")
    for env in envs:
        print(f"  - {env} -> {envBoardMap[env]}")
    print(f"Output: {targetProjectDir}")
    print(f"Workspace dir: {workspaceDir}")

    for env in envs:
        boardName = envBoardMap[env]
        if boardCounts[boardName] > 1:
            envVersionDir = targetProjectDir / env / boardName / version
        else:
            envVersionDir = targetProjectDir / boardName / version
        envVersionDir.mkdir(parents=True, exist_ok=True)

        logLines: list[str] = []
        runCommand(["pio", "run", "-e", env], projectPath, logLines)

        if (projectPath / "data").is_dir():
            try:
                runCommand(["pio", "run", "-e", env, "-t", "buildfs"], projectPath, logLines)
            except RuntimeError as exc:
                logLines.append(f"WARN: buildfs niet gelukt voor {env}: {exc}")

        envPartitionsSource = resolveEnvPartitionsSource(projectPath, platformioSections, env)

        collectAndCopyArtifacts(
            projectPath,
            workspaceDir,
            env,
            envVersionDir,
            envPartitionsSource,
            version,
            logLines,
        )
        print(f"Completed for env '{env}': {envVersionDir}")

    if args.sync_aws:
        awsSshKey = Path(defaultAwsSshKey).expanduser().resolve()
        if not awsSshKey.exists():
            raise SystemExit(f"SSH key not found: {awsSshKey}")
        syncProjectToAws(
            projectsRoot=projectsRoot,
            projectName=projectName,
            awsServer=defaultAwsServer,
            awsTarget=defaultAwsTarget,
            awsSshKey=awsSshKey,
            awsDryRun=args.aws_dry_run,
        )

    print("Structure created successfully.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit("Aborted by user.")
    