#!/usr/bin/env python3
"""Generate Android example targets and launcher metadata."""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
EXAMPLES_CMAKE = REPO_ROOT / "Examples" / "CMakeLists.txt"
MAIN_ACTIVITY = REPO_ROOT / "Android" / "app" / "src" / "main" / "java" / "foundation" / "examples" / "MainActivity.java"
BUILD_GRADLE = REPO_ROOT / "Android" / "app" / "build.gradle.kts"

ANDROID_EXCLUDED_EXAMPLES = {
    "Example_HeadlessTriangle",
    "Example_HeadlessPathTracer",
    "Example_GPUSceneGLTF",
}


@dataclass(frozen=True)
class Example:
    target: str
    source: Path
    description: str

    @property
    def title(self) -> str:
        return self.target.removeprefix("Example_")


def parse_examples(cmake_path: Path) -> list[Example]:
    cmake = cmake_path.read_text(encoding="utf-8")
    examples: list[Example] = []
    for match in re.finditer(r"add_example\(\s*(\w+)\s+\"([^\"]+)\"\s*\)", cmake):
        target = match.group(1)
        if target in ANDROID_EXCLUDED_EXAMPLES:
            continue

        source = cmake_path.parent / match.group(2)
        examples.append(Example(target=target, source=source, description=read_source_description(source)))

    return sorted(examples, key=example_sort_key)


def example_sort_key(example: Example) -> tuple[str, str, str]:
    return (example.description.casefold(), example.title.casefold(), example.target.casefold())


def read_source_description(source: Path, max_lines: int = 5) -> str:
    lines = source.read_text(encoding="utf-8").splitlines()
    description: list[str] = []

    for line in lines:
        stripped = line.strip()
        if not stripped:
            if description:
                break
            continue
        if not stripped.startswith("//"):
            break

        text = stripped[2:].strip()
        if not text:
            if description:
                break
            continue

        description.append(text)
        if len(description) >= max_lines:
            break

    if description and description[0].lower().startswith("example:"):
        description[0] = description[0].split(":", 1)[1].strip()

    return " ".join(description)


def java_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render_main_activity_examples(examples: list[Example]) -> str:
    lines = ["        Example[] examples = {"]
    for index, example in enumerate(examples):
        comma = "," if index + 1 < len(examples) else ""
        lines.append(
            "            new Example("
            f"{java_string(example.target)}, "
            f"{java_string(example.title)}, "
            f"{java_string(example.description)}){comma}"
        )
    lines.append("        };")
    return "\n".join(lines)


def render_gradle_targets(examples: list[Example]) -> str:
    lines = ["                targets("]
    for index, example in enumerate(examples):
        comma = "," if index + 1 < len(examples) else ""
        lines.append(f'                    "{example.target}"{comma}')
    lines.append("                )")
    return "\n".join(lines)


def replace_generated_region(content: str, start_marker: str, end_marker: str, generated: str) -> str:
    pattern = re.compile(
        rf"(?P<indent>[ \t]*){re.escape(start_marker)}\n.*?\n(?P=indent){re.escape(end_marker)}",
        re.DOTALL,
    )

    def replacement(match: re.Match[str]) -> str:
        indent = match.group("indent")
        return f"{indent}{start_marker}\n{generated}\n{indent}{end_marker}"

    updated, count = pattern.subn(replacement, content)
    if count != 1:
        raise RuntimeError(f"Expected one generated region delimited by {start_marker!r} and {end_marker!r}, found {count}")
    return updated


def update_file(path: Path, content: str, dry_run: bool) -> bool:
    old_bytes = path.read_bytes()
    old = old_bytes.decode("utf-8")
    if old.replace("\r\n", "\n") == content:
        return False
    if not dry_run:
        newline = "\r\n" if b"\r\n" in old_bytes else "\n"
        path.write_text(content, encoding="utf-8", newline=newline)
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true", help="Report whether files would change without writing them.")
    args = parser.parse_args()

    examples = parse_examples(EXAMPLES_CMAKE)

    main_activity = MAIN_ACTIVITY.read_text(encoding="utf-8")
    main_activity = replace_generated_region(
        main_activity,
        "        // BEGIN GENERATED ANDROID EXAMPLES",
        "        // END GENERATED ANDROID EXAMPLES",
        render_main_activity_examples(examples),
    )

    build_gradle = BUILD_GRADLE.read_text(encoding="utf-8")
    build_gradle = replace_generated_region(
        build_gradle,
        "                // BEGIN GENERATED ANDROID EXAMPLE TARGETS",
        "                // END GENERATED ANDROID EXAMPLE TARGETS",
        render_gradle_targets(examples),
    )

    changed = [
        path
        for path, content in ((MAIN_ACTIVITY, main_activity), (BUILD_GRADLE, build_gradle))
        if update_file(path, content, args.dry_run)
    ]

    if args.dry_run:
        for path in changed:
            print(f"would update {path.relative_to(REPO_ROOT)}")
    else:
        for path in changed:
            print(f"updated {path.relative_to(REPO_ROOT)}")

    return 1 if args.dry_run and changed else 0


if __name__ == "__main__":
    raise SystemExit(main())
