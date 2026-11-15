"""mcgen pseudo: javap bytecode -> compact pseudo-code, for reading vanilla *logic* (stdlib only).

`javap -c` prints one line per instruction; a 60-line vanilla method becomes 400 lines of stack
shuffling. Since 26.1 the jars keep local variable names, so a symbolic stack is enough to turn
that back into one line per statement with the real names:

    lightEmission = this.getEmission(blockNode, state);
    oldLevel = this.storage.getStoredLevel(blockNode);
    if (lightEmission >= oldLevel) goto L88;

This is a reading aid, not a decompiler: control flow stays as labels and gotos, a value that is
merged from two branches (a ternary) is shown as {a | b}, and anything unexpected is passed through
as a raw instruction rather than guessed at. When a line looks wrong, `javap --full` is the truth.
"""
from __future__ import annotations

import re
import sys

_PKG = re.compile(r"(?<![\w$])(?:[a-z_][\w$]*[/.])+(?=[A-Za-z_$])")


def short(name: str) -> str:
    """net/minecraft/core/BlockPos$MutableBlockPos -> BlockPos$MutableBlockPos"""
    return _PKG.sub("", name)


def parse_desc(desc: str) -> tuple[list[str], str]:
    """'(IJLfoo/Bar;[I)V' -> (['I', 'J', 'Lfoo/Bar;', '[I'], 'V')"""
    assert desc.startswith("("), desc
    i = 1
    args: list[str] = []
    while desc[i] != ")":
        j = i
        while desc[j] == "[":
            j += 1
        if desc[j] == "L":
            j = desc.index(";", j)
        args.append(desc[i:j + 1])
        i = j + 1
    return args, desc[i + 1:]


class Val:
    """One operand-stack entry. `cat` is the JVM category (2 for long/double)."""
    __slots__ = ("s", "cat", "is_bool", "atom", "new")

    def __init__(self, s: str, cat: int = 1, is_bool: bool = False, atom: bool = True, new: str | None = None):
        self.s, self.cat, self.is_bool, self.atom, self.new = s, cat, is_bool, atom, new

    def p(self) -> str:  # as an operand of something that binds tighter
        return self.s if self.atom else f"({self.s})"


def _cat(t: str) -> int:
    return 2 if t in ("J", "D") else 1


_BIN = {"add": "+", "sub": "-", "mul": "*", "div": "/", "rem": "%", "shl": "<<", "shr": ">>", "ushr": ">>>",
        "and": "&", "or": "|", "xor": "^"}
_IF0 = {"ifeq": "==", "ifne": "!=", "iflt": "<", "ifge": ">=", "ifgt": ">", "ifle": "<="}
_IFCMP = {"eq": "==", "ne": "!=", "lt": "<", "ge": ">=", "gt": ">", "le": "<="}
_CAST = {"i2b": "byte", "i2c": "char", "i2s": "short", "l2i": "int", "f2i": "int", "d2i": "int", "f2l": "long",
         "d2l": "long", "d2f": "float", "i2f": "float", "l2f": "float", "i2d": "double", "l2d": "double", "f2d": "double"}
_ARR = {"4": "boolean", "5": "char", "6": "float", "7": "double", "8": "byte", "9": "short", "10": "int", "11": "long"}


class Method:
    def __init__(self, header: str):
        self.header = header
        self.ins: list[tuple[int, str, str, str]] = []  # pc, opcode, operands, comment
        self.switches: dict[int, list[tuple[str, int]]] = {}
        self.lvt: list[tuple[int, int, int, str, str]] = []  # start, length, slot, name, signature
        self.handlers: list[tuple[int, int, int, str]] = []
        self.static = bool(re.search(r"\bstatic\b", header.split("(")[0]))


def parse_classes(text: str):
    """Yield (class_header, fields, methods, bootstrap) per class in `javap -v -p -c -l -constants` output."""
    classes = []
    cur = None
    method = None
    mode = ""
    switch_pc = None
    boot_idx = None
    lines = text.splitlines()
    for raw in lines:
        if not raw.strip():
            continue
        if not raw.startswith(" ") and re.search(r"\b(class|interface|enum)\b", raw) and not raw.startswith("Classfile"):
            cur = {"header": raw.rstrip(" {"), "fields": [], "methods": [], "boot": {}}
            classes.append(cur)
            method, mode = None, ""
            continue
        if cur is None:
            continue
        if raw.startswith("Constant pool:"):
            mode, method = "pool", None
            continue
        if mode == "pool":
            if raw.startswith("{"):
                mode = ""
            continue
        if raw.startswith("BootstrapMethods:"):
            mode, method = "boot", None
            continue
        if not raw.startswith(" ") and mode == "boot":
            mode = ""
        if mode == "boot":
            m = re.match(r"^  (\d+): #\d+ REF_\w+ (\S+)", raw)
            if m:
                boot_idx = int(m.group(1))
                cur["boot"][boot_idx] = "concat" if "StringConcatFactory" in m.group(2) else "?"
                continue
            m = re.match(r"^      #\d+ REF_\w+ ([^:\s]+):", raw)
            if m and boot_idx is not None and cur["boot"].get(boot_idx) == "?":
                cur["boot"][boot_idx] = m.group(1)
            continue
        if re.match(r"^  \S", raw):  # member header
            method, mode = None, ""
            line = raw.strip()
            if "(" in line and line.endswith(";") or line.startswith("static {}"):
                method = Method(line.rstrip(";"))
                cur["methods"].append(method)
            elif line.endswith(";"):
                cur["fields"].append(line)
            continue
        if method is None:
            continue
        s = raw.strip()
        if s == "Code:":
            mode = "code"
            continue
        if s.startswith("Exception table:"):
            mode = "exc"
            continue
        if s.startswith("LocalVariableTable:"):
            mode = "lvt"
            continue
        if re.match(r"^(LineNumberTable|LocalVariableTypeTable|StackMapTable|Signature|MethodParameters|Runtime\w+|"
                    r"Exceptions|descriptor|flags|Deprecated|AnnotationDefault|ConstantValue)\b", s):
            mode = "skip" if not s.startswith(("descriptor", "flags")) else mode
            continue
        if mode == "code":
            if switch_pc is not None:
                m = re.match(r"^(-?\d+|default): (\d+)$", s)
                if m:
                    method.switches[switch_pc].append((m.group(1), int(m.group(2))))
                elif s == "}":
                    switch_pc = None
                continue
            m = re.match(r"^(\d+): (\w+)\s*(.*?)\s*(?://\s*(.*))?$", s)
            if m:
                pc, op, args, comment = int(m.group(1)), m.group(2), m.group(3), m.group(4) or ""
                method.ins.append((pc, op, args, comment))
                if op in ("tableswitch", "lookupswitch"):
                    switch_pc = pc
                    method.switches[pc] = []
        elif mode == "lvt":
            m = re.match(r"^(\d+)\s+(\d+)\s+(\d+)\s+(\S+)\s+(\S+)$", s)
            if m:
                method.lvt.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4), m.group(5)))
        elif mode == "exc":
            m = re.match(r"^(\d+)\s+(\d+)\s+(\d+)\s+(.+)$", s)
            if m:
                method.handlers.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), short(m.group(4).replace("Class ", ""))))
    return classes


def _const(comment: str, args: str) -> Val:
    m = re.match(r"^(String|int|long|float|double|class) ?(.*)$", comment, re.S)
    if not m:
        return Val(short(comment) or args)
    kind, v = m.group(1), m.group(2)
    if kind == "String":
        return Val('"' + v + '"')
    if kind == "class":
        return Val(short(v.strip('"')) + ".class")
    return Val(v, cat=2 if kind in ("long", "double") else 1)


def render(method: Method, boot: dict[int, str]) -> list[str]:
    ins = method.ins
    out: list[str] = []
    if not ins:
        return out
    next_pc = {ins[i][0]: (ins[i + 1][0] if i + 1 < len(ins) else ins[i][0] + 1) for i in range(len(ins))}
    targets: set[int] = set()
    for pc, op, args, _ in ins:
        if op.startswith(("if", "goto")) and args.isdigit():
            targets.add(int(args))
        if pc in method.switches:
            targets.update(t for _, t in method.switches[pc])
    targets.update(h[2] for h in method.handlers)
    handler_at = {h[2]: h for h in method.handlers}

    def local(slot: int, pc: int) -> tuple[str, str]:
        for start, length, s, name, sig in method.lvt:
            if s == slot and start <= pc < start + length:
                return name, sig
        for start, length, s, name, sig in method.lvt:  # a store happens just before the range starts
            if s == slot and pc < start <= pc + 4:
                return name, sig
        return f"v{slot}", "?"

    stack: list[Val] = []
    saved: dict[int, list[Val]] = {}
    dead = False  # previous instruction never falls through

    def pop() -> Val:
        return stack.pop() if stack else Val("?")

    def emit(s: str) -> None:
        out.append("    " + s)

    def jump(target: int) -> None:
        if target not in saved:
            saved[target] = list(stack)

    for pc, op, args, comment in ins:
        if pc in targets:
            if pc in handler_at:
                stack = [Val(f"caught<{handler_at[pc][3]}>")]
            elif dead:
                stack = list(saved.get(pc, []))
            elif pc in saved and saved[pc] and stack and len(saved[pc]) == len(stack):
                a, b = saved[pc][-1], stack[-1]
                if a.s != b.s:  # a value merged from two branches: javac's ternary / short-circuit shape
                    stack[-1] = Val("{" + a.s + " | " + b.s + "}", b.cat, a.is_bool and b.is_bool)
            out.append(f"  L{pc}:")
        dead = False
        npc = next_pc[pc]
        base = re.sub(r"_(w|\d|m1)$", "", op) if re.match(r"^[ilfda](load|store)_\d$|^[ilfd]const_", op) else op

        # ---- constants and locals
        if op == "aconst_null":
            stack.append(Val("null"))
        elif re.match(r"^[ilfd]const_", op):
            v = op.split("_", 1)[1]
            stack.append(Val("-1" if v == "m1" else v, cat=2 if op[0] in "ld" else 1))
        elif op in ("bipush", "sipush"):
            stack.append(Val(args))
        elif op in ("ldc", "ldc_w", "ldc2_w"):
            stack.append(_const(comment, args))
        elif re.match(r"^[ilfda]load(_\d)?$", op):
            slot = int(op[-1]) if "_" in op else int(args)
            name, sig = local(slot, pc)
            stack.append(Val(name, cat=2 if op[0] in "ld" else 1, is_bool=sig == "Z"))
        elif re.match(r"^[ilfda]store(_\d)?$", op):
            slot = int(op[-1]) if "_" in op else int(args)
            name, _ = local(slot, npc)
            emit(f"{name} = {pop().s};")
        elif op == "iinc":
            slot, delta = (int(x) for x in args.split(","))
            name, _ = local(slot, pc)
            emit(f"{name} += {delta};" if delta >= 0 else f"{name} -= {-delta};")
        elif re.match(r"^[ilfdabcs]aload$", op):
            i, a = pop(), pop()
            stack.append(Val(f"{a.p()}[{i.s}]", cat=2 if op[0] in "ld" else 1))
        elif re.match(r"^[ilfdabcs]astore$", op):
            v, i, a = pop(), pop(), pop()
            emit(f"{a.p()}[{i.s}] = {v.s};")

        # ---- stack shuffling
        elif op == "pop" or op == "pop2":
            v = pop()
            if op == "pop2" and v.cat == 1:
                v2 = pop()
                if "(" in v2.s:
                    emit(v2.s + ";")
            if "(" in v.s:
                emit(v.s + ";")
        elif op == "dup":
            stack.append(stack[-1] if stack else Val("?"))
        elif op == "dup_x1":
            a, b = pop(), pop()
            stack += [a, b, a]
        elif op == "dup_x2":
            a, b = pop(), pop()
            if b.cat == 2:
                stack += [a, b, a]
            else:
                c = pop()
                stack += [a, c, b, a]
        elif op == "dup2":
            a = pop()
            if a.cat == 2:
                stack += [a, a]
            else:
                b = pop()
                stack += [b, a, b, a]
        elif op == "dup2_x1":
            a = pop()
            if a.cat == 2:
                b = pop()
                stack += [a, b, a]
            else:
                b, c = pop(), pop()
                stack += [b, a, c, b, a]
        elif op == "dup2_x2":
            a = pop()
            if a.cat == 2:
                b = pop()
                if b.cat == 2:
                    stack += [a, b, a]
                else:
                    c = pop()
                    stack += [a, c, b, a]
            else:
                b, c = pop(), pop()
                if c.cat == 2:
                    stack += [b, a, c, b, a]
                else:
                    d = pop()
                    stack += [b, a, d, c, b, a]
        elif op == "swap":
            a, b = pop(), pop()
            stack += [a, b]

        # ---- arithmetic
        elif len(op) > 1 and op[0] in "ilfd" and op[1:] in _BIN:
            b, a = pop(), pop()
            stack.append(Val(f"{a.p()} {_BIN[op[1:]]} {b.p()}", cat=2 if op[0] in "ld" else 1, atom=False))
        elif op in ("ineg", "lneg", "fneg", "dneg"):
            stack.append(Val(f"-{pop().p()}", cat=2 if op[0] in "ld" else 1))
        elif op in _CAST:
            a = pop()
            stack.append(Val(f"({_CAST[op]}){a.p()}", cat=2 if op[-1] in "ld" else 1))
        elif op == "i2l":
            a = pop()
            stack.append(Val(a.s, cat=2, atom=a.atom))
        elif op in ("lcmp", "fcmpl", "fcmpg", "dcmpl", "dcmpg"):
            b, a = pop(), pop()
            v = Val(f"cmp({a.s}, {b.s})")
            v.new = f"{a.p()}\0{b.p()}"  # remembered so the following if<cond> can print a real comparison
            stack.append(v)

        # ---- control flow
        elif op in _IF0:
            a = pop()
            t = int(args)
            if a.new and a.s.startswith("cmp("):
                x, y = a.new.split("\0")
                emit(f"if ({x} {_IF0[op]} {y}) goto L{t};")
            elif a.is_bool and op in ("ifeq", "ifne"):
                emit(f"if ({'!' if op == 'ifeq' else ''}{a.p() if op == 'ifeq' else a.s}) goto L{t};")
            else:
                emit(f"if ({a.p()} {_IF0[op]} 0) goto L{t};")
            jump(t)
        elif op.startswith(("if_icmp", "if_acmp")):
            b, a = pop(), pop()
            t = int(args)
            emit(f"if ({a.p()} {_IFCMP[op[-2:]]} {b.p()}) goto L{t};")
            jump(t)
        elif op in ("ifnull", "ifnonnull"):
            a = pop()
            t = int(args)
            emit(f"if ({a.p()} {'==' if op == 'ifnull' else '!='} null) goto L{t};")
            jump(t)
        elif op in ("goto", "goto_w"):
            t = int(args)
            jump(t)
            if stack:
                emit(f"goto L{t};  // carrying {stack[-1].s}")
            else:
                emit(f"goto L{t};")
            dead = True
        elif op in ("tableswitch", "lookupswitch"):
            a = pop()
            cases = ", ".join(f"{k}: L{t}" for k, t in method.switches.get(pc, []))
            emit(f"switch ({a.s}) {{ {cases} }}")
            for _, t in method.switches.get(pc, []):
                jump(t)
            dead = True
        elif op in ("ireturn", "lreturn", "freturn", "dreturn", "areturn"):
            emit(f"return {pop().s};")
            dead = True
        elif op == "return":
            if pc != ins[-1][0]:
                emit("return;")
            dead = True
        elif op == "athrow":
            emit(f"throw {pop().s};")
            dead = True

        # ---- fields
        elif op in ("getstatic", "getfield", "putstatic", "putfield"):
            m = re.match(r"^Field (?:(\S+)\.)?([\w$<>]+):(\S+)$", comment)
            owner, name, ftype = (m.group(1), m.group(2), m.group(3)) if m else (None, comment, "?")
            if op == "getstatic":
                stack.append(Val(f"{short(owner)}.{name}" if owner else name, _cat(ftype), ftype == "Z"))
            elif op == "getfield":
                stack.append(Val(f"{pop().p()}.{name}", _cat(ftype), ftype == "Z"))
            elif op == "putstatic":
                emit(f"{short(owner) + '.' if owner else ''}{name} = {pop().s};")
            else:
                v, o = pop(), pop()
                emit(f"{o.p()}.{name} = {v.s};")

        # ---- calls
        elif op in ("invokevirtual", "invokespecial", "invokestatic", "invokeinterface"):
            m = re.match(r"^(?:Interface)?Method (?:(\"?\[?[\w/$;\[]+\"?)\.)?\"?([\w$<>]+)\"?:(\S+)$", comment)
            if not m:
                emit(f"<{op} {comment}>")
                stack.clear()
                continue
            owner, name, desc = m.group(1), m.group(2), m.group(3)
            argt, ret = parse_desc(desc)
            argv = [pop() for _ in argt][::-1]
            arglist = ", ".join(a.s for a in argv)
            if op == "invokestatic":
                call = f"{short(owner) + '.' if owner else ''}{name}({arglist})"
            else:
                obj = pop()
                if name == "<init>":
                    if obj.new:
                        obj.s, obj.new = f"new {obj.new}({arglist})", None  # the dup'ed copy shares this Val
                        continue
                    emit(f"{'this' if owner is None else 'super'}({arglist});")
                    continue
                prefix = "super." if op == "invokespecial" and owner and obj.s == "this" else ""
                call = f"{prefix + name if prefix else obj.p() + '.' + name}({arglist})"
            if ret == "V":
                emit(call + ";")
            else:
                stack.append(Val(call, _cat(ret), ret == "Z"))
        elif op == "invokedynamic":
            m = re.match(r"^InvokeDynamic #(\d+):([\w$]+):(\S+)$", comment)
            if not m:
                emit(f"<{op} {comment}>")
                stack.clear()
                continue
            argt, ret = parse_desc(m.group(3))
            argv = [pop() for _ in argt][::-1]
            target = boot.get(int(m.group(1)), "?")
            if target == "concat":
                stack.append(Val("concat(" + ", ".join(a.s for a in argv) + ")"))
            else:
                cap = f"[{', '.join(a.s for a in argv)}]" if argv else ""
                stack.append(Val(f"&{short(target)}{cap}"))

        # ---- objects and arrays
        elif op == "new":
            stack.append(Val("new?", new=short(comment.replace("class ", "").strip('"'))))
        elif op == "newarray":
            stack.append(Val(f"new {_ARR.get(args, args)}[{pop().s}]"))
        elif op == "anewarray":
            stack.append(Val(f"new {short(comment.replace('class ', '').strip(chr(34)))}[{pop().s}]"))
        elif op == "multianewarray":
            dims = int(args.split(",")[-1])
            sizes = [pop().s for _ in range(dims)][::-1]
            stack.append(Val(f"new {short(comment.replace('class ', '').strip(chr(34)))}" + "".join(f"[{s}]" for s in sizes)))
        elif op == "arraylength":
            stack.append(Val(f"{pop().p()}.length"))
        elif op == "checkcast":
            pass  # casts carry no logic; dropping them keeps lines short
        elif op == "instanceof":
            a = pop()
            stack.append(Val(f"{a.p()} instanceof {short(comment.replace('class ', '').strip(chr(34)))}", is_bool=True, atom=False))
        elif op in ("monitorenter", "monitorexit"):
            emit(f"{op}({pop().s});")
        elif op == "nop":
            pass
        else:
            emit(f"<{op} {args} {comment}>".replace("  ", " "))
            stack.clear()
    return out


def pseudo(text: str, method_rx: str | None, fields: bool = True) -> str:
    rx = re.compile(method_rx) if method_rx else None
    out: list[str] = []
    for cls in parse_classes(text):
        out.append(short(cls["header"]))
        if fields and not rx:
            out.extend("  " + short(f) for f in cls["fields"])
        for m in cls["methods"]:
            if rx and not rx.search(m.header):
                continue
            body = render(m, cls["boot"])
            out.append("  " + short(m.header) + (" {" if body else ";"))
            out.extend(body)
            if body:
                out.append("  }")
    return "\n".join(out)


def cmd_pseudo(args) -> None:
    import subprocess
    from mcgen import die, find_tool, jar_for, version_info
    tool = find_tool("javap", version_info(args.version)["java_version"])
    res = subprocess.run([tool, "-v", "-p", "-c", "-l", "-constants", "-cp", str(jar_for(args.version, args.client)), *args.classes],
                         capture_output=True, text=True)
    if res.returncode != 0:
        die(f"javap failed: {res.stderr.strip()}")
    sys.stdout.write(pseudo(res.stdout, args.method) + "\n")
