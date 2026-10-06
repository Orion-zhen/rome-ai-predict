"""安装到临时 Rime 目录，用官方 librime 部署并加载未替换的正式 Lua/DLL。"""
import ctypes as C
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

class Traits(C.Structure):
    _fields_ = [("data_size", C.c_int)] + [(name, C.c_char_p) for name in
        ("shared_data_dir", "user_data_dir", "distribution_name", "distribution_code_name",
         "distribution_version", "app_name")] + [
        ("modules", C.POINTER(C.c_char_p)), ("min_log_level", C.c_int), ("log_dir", C.c_char_p),
        ("prebuilt_data_dir", C.c_char_p), ("staging_dir", C.c_char_p)]


class Commit(C.Structure):
    _fields_ = [("data_size", C.c_int), ("text", C.c_char_p)]


def bind(dll, name, result, *args):
    function = getattr(dll, name)
    function.restype, function.argtypes = result, list(args)
    return function


def prepare_fixture(root):
    (root / "default.yaml").write_text("config_version: '1'\nschema_list: [{schema: rome}]\n", encoding="utf-8")
    (root / "rome.schema.yaml").write_text('''schema:
  schema_id: rome
  name: Isolated input
  version: '1'
switches:
  - name: ascii_mode
    reset: 0
engine:
  processors: [speller, selector, express_editor]
  segmentors: [abc_segmentor, fallback_segmentor]
  translators: [script_translator]
translator:
  dictionary: rome
speller:
  alphabet: abcdefghijklmnopqrstuvwxyz
''', encoding="utf-8")
    (root / "rome.dict.yaml").write_text(
        "---\nname: rome\nversion: '1'\nsort: by_weight\n...\n你好\tni hao\t1000\n去\tqu\t1000\n", encoding="utf-8")


def worker(rime_file, root):
    rime = C.CDLL(str(rime_file))
    setup = bind(rime, "RimeSetup", None, C.POINTER(Traits))
    initialize = bind(rime, "RimeInitialize", None, C.POINTER(Traits))
    finalize = bind(rime, "RimeFinalize", None)
    deploy_config = bind(rime, "RimeDeployConfigFile", C.c_int, C.c_char_p, C.c_char_p)
    deploy_schema = bind(rime, "RimeDeploySchema", C.c_int, C.c_char_p)
    create = bind(rime, "RimeCreateSession", C.c_size_t)
    destroy = bind(rime, "RimeDestroySession", C.c_int, C.c_size_t)
    choose = bind(rime, "RimeSelectSchema", C.c_int, C.c_size_t, C.c_char_p)
    process = bind(rime, "RimeProcessKey", C.c_int, C.c_size_t, C.c_int, C.c_int)
    get_option = bind(rime, "RimeGetOption", C.c_int, C.c_size_t, C.c_char_p)
    set_option = bind(rime, "RimeSetOption", None, C.c_size_t, C.c_char_p, C.c_int)
    get_property = bind(rime, "RimeGetProperty", C.c_int, C.c_size_t, C.c_char_p, C.c_void_p, C.c_size_t)
    set_property = bind(rime, "RimeSetProperty", None, C.c_size_t, C.c_char_p, C.c_char_p)
    get_commit = bind(rime, "RimeGetCommit", C.c_int, C.c_size_t, C.POINTER(Commit))
    free_commit = bind(rime, "RimeFreeCommit", C.c_int, C.POINTER(Commit))
    modules = (C.c_char_p * 4)(b"default", b"deployer", b"lua", None)
    traits = Traits()
    traits.data_size = C.sizeof(traits) - C.sizeof(C.c_int)
    traits.user_data_dir = traits.shared_data_dir = str(root).encode("utf-8")
    traits.modules, traits.app_name, traits.min_log_level, traits.log_dir = modules, b"rime.rome-package-test", 0, b""
    setup(C.byref(traits))
    initialize(C.byref(traits))
    sid = 0
    try:
        assert deploy_config(b"default.yaml", b"config_version")
        assert deploy_schema(str(root / "rome.schema.yaml").encode("utf-8")), "full schema deployment failed"
        sid = create()
        assert sid and choose(sid, b"rome")
        def status():
            value = C.create_string_buffer(128)
            assert get_property(sid, b"rome_ai_predict/status", value, len(value))
            return value.value.decode()
        def chinese():
            for pinyin, expected in [("nihao", "你好"), ("qu", "去")]:
                for letter in pinyin:
                    assert process(sid, ord(letter), 0)
                assert process(sid, ord(" "), 0)
                value = Commit()
                value.data_size = C.sizeof(value) - C.sizeof(C.c_int)
                assert get_commit(sid, C.byref(value))
                try:
                    assert value.text.decode() == expected
                finally:
                    free_commit(C.byref(value))
        assert status() == "disabled", f"installed loader/DLL did not honor disabled configuration: {status()}"
        assert not process(sid, 0xFFD5, 0), "disabled adapter reserved F24"
        chinese()
        set_option(sid, b"rome_ai_predict", 1)
        assert not get_option(sid, b"rome_ai_predict") and status() == "configuration-error", "empty model was accepted"
        chinese()
        configuration = {"enabled": False, "base_url": "http://127.0.0.1:9/v1", "model": "unused-package-test"}
        user = root / "rome-ai-predict.yaml"
        user.write_text(json.dumps(configuration), encoding="utf-8")
        set_option(sid, b"rome_ai_predict", 1)
        assert get_option(sid, b"rome_ai_predict"), "runtime enable did not reload user overrides"
        assert process(sid, 0xFFD5, 0), "installed enabled adapter did not consume F24"
        assert process(sid, 0xFFD5, 1 << 30), "F24 release was not consumed"
        # 没有 client_app/client_type，正式宿主须在读取桌面正文前拒绝；普通拼音仍可上屏。
        chinese()
        activity = (root / "lifecycle.txt").read_text(encoding="ascii")
        assert re.fullmatch(r"[wAc01]+", activity), "unexpected lifecycle metadata"
        committed = activity.index("c")
        next_arm = activity.index("A", committed)
        assert "1" in activity[:committed] and "0" in activity[committed:next_arm], "Rime composition lifecycle did not reach native session"
        start = len(activity)
        process(sid, 0xFF51, 0)
        process(sid, 0xFF51, 1 << 30)
        navigation = (root / "lifecycle.txt").read_text(encoding="ascii")[start:]
        assert "A" in navigation and navigation.endswith("0"), "non-composing key release did not restore idle sampling"
        set_option(sid, b"rome_ai_predict", 0)
        assert not get_option(sid, b"rome_ai_predict") and status() == "disabled"
        configuration["unknown_secret_key"] = "must-not-appear-in-status"
        user.write_text(json.dumps(configuration), encoding="utf-8")
        set_option(sid, b"rome_ai_predict", 1)
        assert not get_option(sid, b"rome_ai_predict") and status() == "configuration-error"
        chinese()
        del configuration["unknown_secret_key"]
        configuration["enabled"] = True
        user.write_text(json.dumps(configuration), encoding="utf-8")
        destroy(sid)
        sid = create()
        assert sid and choose(sid, b"rome")
        assert process(sid, 0xFFD5, 0) and get_option(sid, b"rome_ai_predict"), "schema reset masked enabled startup configuration"
        chinese()
        for app in (b"CHROME.EXE", b"msedge.exe", b"Discord.exe", b"notepad.exe", b"private-app-must-not-log"):
            destroy(sid)
            sid = create()
            assert sid and choose(sid, b"rome")
            # 非 TSF 客户端在捕获目标前被拒绝；这些会话不会读取桌面或发送按键。
            set_property(sid, b"client_type", b"private-client-must-not-log")
            set_property(sid, b"client_app", app)
            assert process(sid, 0xFFD5, 0) and get_option(sid, b"rome_ai_predict")
            chinese()
        module = C.CDLL(str(root / "rome-ai-predict" / "rome-ai-predict-weasel.dll"))
        assert hasattr(module, "luaopen_rome_ai_predict")
    finally:
        if sid:
            destroy(sid)
        finalize()
    assert not list(root.glob("*.userdb"))
    assert not list((root / "rome-ai-predict").glob("*.log")), "unexpected logging in installed package"
    print("PASS: installed production loader/DLL, schema deployment, startup/toggle/reload/error, normal Chinese input")


def main():
    if sys.argv[1] == "--worker":
        worker(Path(sys.argv[2]), Path(sys.argv[3]))
        return
    cmake, build, config, rime = sys.argv[1:]
    rime = Path(rime).resolve()
    official_hash = hashlib.sha256(rime.read_bytes()).digest()
    with tempfile.TemporaryDirectory(prefix="rome-weasel-install-") as temporary:
        for name in ("user with spaces", "用户", "用户😀"):
            root = Path(temporary) / name
            try:
                str(root).encode("mbcs", errors="strict")
            except UnicodeEncodeError:
                print("SKIP: path is not representable in the standalone host ACP; no Unicode-path claim for this case")
                continue
            root.mkdir()
            prepare_fixture(root)
            (root / "rome.custom.yaml").write_text(json.dumps({"patch": {
                "engine/processors/@before 0": "lua_processor@*rome_ai_predict",
                "switches/@next": {"name": "rome_ai_predict", "reset": 0, "states": ["AI关", "AI开"]},
                "translator/enable_user_dict": False,
            }}), encoding="utf-8")
            (root / "rome-ai-predict.yaml").write_text("# user settings must survive installation\nenabled: false\n", encoding="utf-8")
            originals = {path: path.read_bytes() for path in root.iterdir() if path.is_file()}
            subprocess.run([cmake, "--install", build, "--config", config, "--prefix", str(root)], check=True)
            assert all(path.read_bytes() == content for path, content in originals.items()), "install overwrote user configuration"
            payload = {str(path.relative_to(root)).replace("\\", "/") for path in root.rglob("*") if path.is_file() and path not in originals}
            assert payload == {"lua/rome_ai_predict.lua", "lua/rome_ai_predict_native.lua",
                               "rome-ai-predict/rome-ai-predict-weasel.dll", "rome-ai-predict/rome-ai-predict.defaults.yaml"}, payload
            (root / "rime.lua").write_text('''local n = require("rome_ai_predict_native")
local function note(value)
  local f = assert(io.open(rime_api.get_user_data_dir() .. "/lifecycle.txt", "a"))
  f:write(value); f:close()
end
for name, value in pairs({watch="w", arm="A", committed="c"}) do
  local original = n[name]
  n[name] = function(...) note(value); return original(...) end
end
local original = n.updated
n.updated = function(host, composing) note(composing and "1" or "0"); return original(host, composing) end
''', encoding="utf-8")
            result = subprocess.run([sys.executable, str(Path(__file__).resolve()), "--worker", str(rime), str(root)],
                                    capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=25)
            if result.returncode:
                print(result.stdout, result.stderr)
                result.check_returncode()
            assert "rome-ai-diag" not in result.stdout + result.stderr, "temporary Lua diagnostics remain"
            print(result.stdout, end="")
    assert hashlib.sha256(rime.read_bytes()).digest() == official_hash
    print("PASS: staged install preserves user files, supports tested host-encoded paths, ships no helper/test executable or extra Lua runtime")


if __name__ == "__main__":
    main()
