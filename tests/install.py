#!/usr/bin/env python3
"""验证独立源码、用户安装、前缀重定位和 DESTDIR 系统安装。"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

source, work, cmake, generator, system_prefix = sys.argv[1:]


def run(*args, env):
    subprocess.run(args, env=env, check=True, timeout=120)


def layout(build, stage=None):
    entries = (build / "install_manifest.txt").read_text(encoding="utf-8").splitlines()
    expected = {"librome-ai-predict.so", "rome-ai-predict.conf", "rome-ai-predict.yaml"}
    assert len(entries) == len(expected), entries
    files = {Path(entry).name: Path(entry) for entry in entries}
    assert set(files) == expected, files
    if stage is not None:
        files = {name: stage / path.relative_to("/") for name, path in files.items()}
    for path in files.values():
        assert path.is_file() and not path.is_symlink(), path
    return files


def library(files):
    text = files["rome-ai-predict.conf"].read_text(encoding="utf-8")
    assert "Version=" not in text, text
    assert "0=core\n" in text, text
    return next(line.removeprefix("Library=") for line in text.splitlines()
                if line.startswith("Library="))


with tempfile.TemporaryDirectory(prefix="install-test-", dir=work) as directory:
    root = Path(directory)
    standalone = root / "source"
    build = root / "build"
    # 只复制源码，排除已有构建产物。
    shutil.copytree(source, standalone,
                    ignore=shutil.ignore_patterns("build*", ".cache", ".git", "__pycache__"))
    env = os.environ.copy()
    home = root / "home"
    home.mkdir()
    env["HOME"] = str(home)
    for variable in ("DESTDIR", "CMAKE_INSTALL_PREFIX", "CMAKE_INSTALL_MODE"):
        env.pop(variable, None)
    for variable in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME"):
        env.pop(variable, None)
    user_config = home / ".config/fcitx5/conf/rome-ai-predict.yaml"
    user_config.parent.mkdir(parents=True)
    user_config.write_text("user configuration must not be overwritten\n", encoding="utf-8")

    run(cmake, "-S", str(standalone), "-B", str(build), "-G", generator,
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_INSTALL_LIBDIR=lib", env=env)
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    assert f"CMAKE_INSTALL_PREFIX:PATH={home / '.local'}\n" in cache
    assert "BUILD_TESTING:BOOL=OFF\n" in cache
    assert "Python3_EXECUTABLE:" not in cache
    assert "RIME_FOUND:" not in cache
    run(cmake, "--build", str(build), "--parallel", "2", env=env)
    assert not (build / "tests").exists(), "普通构建不应生成诊断或测试工具"

    run(cmake, "--install", str(build), env=env)
    user = layout(build)
    assert library(user) == str(user["librome-ai-predict.so"].with_suffix(""))
    assert user["rome-ai-predict.yaml"] == home / ".local/share/fcitx5/conf/rome-ai-predict.yaml"
    assert user["librome-ai-predict.so"] == home / ".local/lib/fcitx5/librome-ai-predict.so"
    assert user_config.read_text(encoding="utf-8") == "user configuration must not be overwritten\n"

    # --prefix 必须在安装时生效，不能留下配置时的用户前缀。
    alternate = root / "alternate prefix"
    run(cmake, "--install", str(build), "--prefix", str(alternate), env=env)
    relocated = layout(build)
    assert library(relocated) == str(relocated["librome-ai-predict.so"].with_suffix(""))
    assert relocated["librome-ai-predict.so"].is_relative_to(alternate)

    user_stage = root / "user-stage"
    run(cmake, "--install", str(build), env={**env, "DESTDIR": str(user_stage)})
    staged_user = layout(build, user_stage)
    assert library(staged_user) == library(user), "DESTDIR must not enter the Library field"

    # 显式的绝对 XDG 数据目录不能随 --prefix 改变，即使它位于原前缀内。
    xdg_data = home / ".local/xdg-data"
    run(cmake, "-S", str(standalone), "-B", str(build),
        f"-DCMAKE_INSTALL_DATADIR={xdg_data}", env=env)
    xdg_prefix = root / "xdg-prefix"
    run(cmake, "--install", str(build), "--prefix", str(xdg_prefix), env=env)
    xdg = layout(build)
    assert xdg["rome-ai-predict.conf"] == xdg_data / "fcitx5/addon/rome-ai-predict.conf"
    assert xdg["rome-ai-predict.yaml"] == xdg_data / "fcitx5/conf/rome-ai-predict.yaml"
    assert library(xdg) == str(xdg["librome-ai-predict.so"].with_suffix(""))

    # 系统安装只写入临时打包目录，不需要 sudo，也不触碰真实 /usr。
    run(cmake, "-S", str(standalone), "-B", str(build),
        f"-DCMAKE_INSTALL_PREFIX={system_prefix}",
        "-DFCITX_INSTALL_USE_FCITX_SYS_PATHS=ON", "-DCMAKE_INSTALL_DATADIR=share", env=env)
    system_stage = root / "system-stage"
    run(cmake, "--install", str(build), env={**env, "DESTDIR": str(system_stage)})
    system = layout(build, system_stage)
    assert library(system) == "librome-ai-predict"
    assert system["rome-ai-predict.yaml"].parent == system["rome-ai-predict.conf"].parents[1] / "conf"

    binary = root / "test-integration"
    runner = root / "run.py"
    probe = root / "rome-ai-probe"
    shutil.copy2(Path(work) / "tests/rome-ai-probe", probe)
    shutil.copy2(Path(work) / "tests/test-integration", binary)
    shutil.copy2(standalone / "tests/run.py", runner)
    standalone.rename(root / "removed-source")
    build.rename(root / "removed-build")
    empty_addons = root / "empty-addons"
    empty_addons.mkdir()

    # 用户安装不得依赖 FCITX_ADDON_DIRS 搜索用户库，系统暂存安装则显式指定暂存库目录。
    for installed, addon_directory in ((user, empty_addons), (relocated, empty_addons),
                                        (xdg, empty_addons),
                                        (system, system["librome-ai-predict.so"].parent)):
        for case in ("automatic", "api-probe", "missing-config", "global-config", "partial-config"):
            run(sys.executable, str(runner), str(binary), str(root), case,
                str(installed["rome-ai-predict.conf"].parents[2]), str(addon_directory),
                str(probe), str(installed["rome-ai-predict.yaml"]), env=env)
    assert user_config.read_text(encoding="utf-8") == "user configuration must not be overwritten\n"
    print("PASS: standalone source, user install, --prefix, DESTDIR, XDG, installed loading")
