#!/usr/bin/env python3
import argparse
import datetime as dt
import json
import re
import shutil
import shlex
import subprocess
import os
from pathlib import Path


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
            logLines.append(f"WARN: partitions.csv parse mislukt: {exc}")

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
                f"WARN: Geen filesystem offset gevonden in partitions.csv voor {filesystemFile}"
            )

    flashPayload = {
        "board": envName,
        "version": version,
        "flash_files": flashFiles,
    }
    (targetVersionDir / "flash.json").write_text(
        json.dumps(flashPayload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


def promptProjectPath(initialValue: str | None) -> Path:
    if initialValue:
        return Path(initialValue).expanduser().resolve()

    entered = input("Geef pad naar PlatformIO project: ").strip()
    if not entered:
        raise SystemExit("Geen pad opgegeven.")
    return Path(entered).expanduser().resolve()


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
            f"Command mislukt ({process.returncode}): {' '.join(cmd)}\n{process.stderr}"
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
        f"Build directory niet gevonden voor env '{envName}' in workspace_dir '{workspaceDir}' of fallback '.pio'."
    )


def findProjectRootPng(projectPath: Path) -> Path | None:
    pngFiles = sorted(projectPath.glob("*.png"))
    if not pngFiles:
        return None

    for pngFile in pngFiles:
        if pngFile.name.lower() == "project.png":
            return pngFile

    return pngFiles[0]


def ensureProjectFiles(projectDir: Path, projectName: str, templateImage: Path | None) -> None:
    projectJson = projectDir / "project.json"
    if not projectJson.exists():
        payload = {
            "name": projectName,
            "long_name_nl": projectName,
            "long_name_en": projectName,
            "description_en": f"Firmware project for {projectName}",
            "description_nl": f"Firmware project voor {projectName}",
            "image": "project.png",
        }
        projectJson.write_text(
            json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )

    projectEn = projectDir / "project_en.md"
    if not projectEn.exists():
        projectEn.write_text(
            f"# {projectName}\n\nEnglish project description.\n", encoding="utf-8"
        )

    projectNl = projectDir / "project_nl.md"
    if not projectNl.exists():
        projectNl.write_text(
            f"# {projectName}\n\nNederlandse projectbeschrijving.\n", encoding="utf-8"
        )

    projectImage = projectDir / "project.png"
    if not projectImage.exists():
        if templateImage and templateImage.exists():
            shutil.copy2(templateImage, projectImage)
        else:
            projectImage.touch()


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
        raise RuntimeError(f"firmware.bin niet gevonden voor env '{envName}'")

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

    raise RuntimeError(f"Executable niet gevonden: {commandName}")


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
        raise RuntimeError(f"Project map bestaat niet voor sync: {sourceProjectPath}")

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
            f"AWS project map aanmaken mislukt: {mkdirProcess.stderr.strip() or mkdirProcess.stdout.strip()}"
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

    print("Start AWS sync van projectmap...")
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
        raise RuntimeError(f"AWS rsync mislukt met code {process.returncode}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Maak projects-structuur vanuit PlatformIO project."
    )
    parser.add_argument(
        "platformioProject",
        nargs="?",
        help="Pad naar PlatformIO project (als leeg, dan interactieve prompt)",
    )
    parser.add_argument(
        "--output-root",
        default=None,
        help="Root waar de map 'projects' staat of aangemaakt wordt (default: platformio project root)",
    )
    parser.add_argument(
        "--sync-aws",
        action="store_true",
        help="Sync gegenereerde projects/<project> map naar AWS (alleen add/update, nooit delete)",
    )
    parser.add_argument(
        "--aws-server",
        default="admin@aandewiel.nl",
        help="SSH server voor AWS sync (default: admin@aandewiel.nl)",
    )
    parser.add_argument(
        "--aws-target",
        default="/home/admin/flasherWebsite_v3",
        help="Remote root path voor website (default: /home/admin/flasherWebsite_v3)",
    )
    parser.add_argument(
        "--aws-ssh-key",
        default="~/.ssh/LightsailDefaultKey-eu-central-1.pem",
        help="Pad naar SSH key voor AWS sync",
    )
    parser.add_argument(
        "--aws-dry-run",
        action="store_true",
        help="Toon AWS rsync wijzigingen zonder daadwerkelijk te kopiëren",
    )
    args = parser.parse_args()

    if shutil.which("pio") is None:
        raise SystemExit(
            "PlatformIO CLI niet gevonden. Installeer PlatformIO Core en zorg dat 'pio' in PATH staat."
        )

    projectPath = promptProjectPath(args.platformioProject)
    if not projectPath.exists() or not projectPath.is_dir():
        raise SystemExit(f"Ongeldig projectpad: {projectPath}")

    os.chdir(projectPath)

    platformioIni = projectPath / "platformio.ini"
    if not platformioIni.exists():
        raise SystemExit(f"platformio.ini niet gevonden in: {projectPath}")

    workspaceDir = getWorkspaceDir(platformioIni, projectPath)
    platformioSections = parsePlatformioSections(platformioIni)

    envs = parseEnvs(platformioIni)
    if not envs:
        raise SystemExit("Geen [env:...] secties gevonden in platformio.ini")

    envBoardMap: dict[str, str] = {}
    boardCounts: dict[str, int] = {}
    for env in envs:
        boardName = resolveEnvBoardName(platformioSections, env)
        envBoardMap[env] = boardName
        boardCounts[boardName] = boardCounts.get(boardName, 0) + 1

    version = detectVersion(projectPath / "src")
    projectName = projectPath.name

    if args.output_root:
        outputRoot = Path(args.output_root).expanduser().resolve()
    else:
        outputRoot = projectPath
    projectsRoot = outputRoot / "projects"
    targetProjectDir = projectsRoot / projectName
    if targetProjectDir.exists():
        print(f"Bestaande projectmap verwijderen: {targetProjectDir}")
        shutil.rmtree(targetProjectDir)
    targetProjectDir.mkdir(parents=True, exist_ok=True)

    templateImage = findProjectRootPng(projectPath)
    ensureProjectFiles(targetProjectDir, projectName, templateImage)

    print(f"Project: {projectName}")
    print(f"Versie: {version}")
    print(f"Environments: {', '.join(envs)}")
    print("Boards per env:")
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
        print(f"Klaar voor env '{env}': {envVersionDir}")

    if args.sync_aws:
        awsSshKey = Path(args.aws_ssh_key).expanduser().resolve()
        if not awsSshKey.exists():
            raise SystemExit(f"SSH key niet gevonden: {awsSshKey}")
        syncProjectToAws(
            projectsRoot=projectsRoot,
            projectName=projectName,
            awsServer=args.aws_server,
            awsTarget=args.aws_target,
            awsSshKey=awsSshKey,
            awsDryRun=args.aws_dry_run,
        )

    print("Structuur succesvol aangemaakt.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit("Afgebroken door gebruiker.")
    