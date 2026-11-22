#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""What does a server answer to commands? Record it, on vanilla and on ours, and diff.

    command_probe.py record <port> <out.json> [nameA nameB [console]] [--only=section,...]
    command_probe.py diff <vanilla.json> <ours.json> [-v]
    tools/vanilla/compare.py command_probe [-k section ...]     # both servers at once, and the diff

`record` joins two bots (tests/integration/mcproto.py, offline mode, creative). A has to be an
operator and B must not be: once both are in, type `op <nameA>` on the server's console when
the probe asks - or name a FIFO that is the server's stdin as `console`, and the probe types
it itself (and `deop` at the end). Not before A has joined: a vanilla server asked to op a name
it has not seen looks it up at Mojang, and ops the account of that name instead of the bot.
A types commands, right ones and wrong ones; B watches, is given things, teleported, opped,
deopped and in the end kicked. Per step everything either of them is told is logged: system
messages (their text components, byte for byte), block changes, teleports, sounds, entities
that appear, the command tree - and chat: what the two type and what the commands that talk say
(player_chat and disguised_chat, field by field; a message's time is its server's own and is
compared as "now"), with the chat shown, cut down to system messages and hidden, and a third
bot that joins, says a word and leaves - and the time: `/time` on a clock that is stopped for it
(so that it is where each command puts it), every set_time that names a clock, and what the
server suggests for the lines a client asks it about. `diff` lines two recordings up step by step.

The reference is the vanilla server, run under the rules in AGENTS.md (gamemode=creative,
level-type=flat, spawn-protection=0):

    cd .cache/vanilla-server && /usr/lib/jvm/java-25-openjdk/bin/java -jar ../mc/26.3/server.jar --nogui
    tools/vanilla/command_probe.py record 25577 /tmp/vanilla.json Eve Fay       # `op Eve` when asked; then stop the server
    build/release/elaine --server-ip=127.0.0.1 --server-port=25580 --online-mode=false --view-distance=2 --max-players=20 --ops-file=/tmp/ops.json
    tools/vanilla/command_probe.py record 25580 /tmp/ours.json Eve Fay          # `op Eve` when asked
    tools/vanilla/command_probe.py diff /tmp/vanilla.json /tmp/ours.json

Reading the diff:
  * Use names vanilla has not seen: it saves players, and the first thing A does is teleport
    both bots to a spot of their own (from the names), so that every coordinate a message names
    is the same on both servers and nothing an earlier recording built is in the way.
  * Offline UUIDs come from the names, so the bots' are the same on both servers; the UUID of
    anything summoned is random and replaced by <uuid>.
  * `help` lists every command a server has: only the lines of commands both have are compared.
    The command tree is compared command by command, for the commands ours has: every node of
    ours must be vanilla's (type, parser and its properties, runs or not, suggestions); what
    vanilla has more is listed, not counted (-v prints it).
  * Left out: the death of what /kill kills (ours vanishes at once; the messages are the
    same), "made the advancement" messages, grass that turns to dirt under
    a block a command put on it (a random tick, whenever it falls), and the particles and sound
    of a block that /setblock destroy breaks (level_event: block_probe.py records those). An
    entity a command made is listed at the end of its step: vanilla shows it at once, ours with
    the tick's other entities.
  * `list` names the most players a server takes: start ours with vanilla's max-players.
  * Known differences are listed in docs/architecture.md ("Commands").
"""
from __future__ import annotations

import json
import re
import struct
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "integration"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import mcproto as mc  # noqa: E402
from inventory_probe import Actor  # noqa: E402

LOGGED = {"system_chat", "player_chat", "disguised_chat", "set_time", "command_suggestions", "block_update", "section_blocks_update", "player_position", "sound", "stop_sound", "add_entity",
          "remove_entities", "entity_event", "commands", "disconnect", "set_entity_data", "container_set_slot",
          "set_player_inventory", "take_item_entity", "open_screen", "block_entity_data", "open_sign_editor", "level_event"}
# The sounds a recording keeps beside the one of an item given: what a lock and a sign say.
SIGN_SOUNDS = {mc.SOUND[k]: k for k in ("kBlockChestLocked", "kItemDyeUse", "kItemInkSacUse", "kItemGlowInkSacUse",
                                        "kItemHoneycombWaxOn", "kBlockSignWaxedInteractFail")}
UUID_RX = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
ROOT = Path(__file__).resolve().parents[2]


NOTHING_SEEN = b"\x00\x00\x00\x00\x00"   # LastSeenMessages.Update: no offset, no bit, no checksum


class Prober(Actor):
    logged = LOGGED

    def say(self, command: str) -> None:
        self.c.send("chat_command", mc.mc_str(command))

    def say_signed(self, command: str) -> None:
        """As a client sends a line with a message argument: chat_command_signed - without
        signatures, as from a client that has no key (offline mode)."""
        self.c.send("chat_command_signed", mc.mc_str(command) + struct.pack(">qq", int(time.time() * 1000), 0) +
                    mc.varint(0) + NOTHING_SEEN)

    def suggest(self, line: str) -> None:
        """command_suggestion: what could come next in this line (as typed, with its slash)?"""
        self.asked = getattr(self, "asked", 0) + 1
        self.c.send("command_suggestion", mc.varint(self.asked) + mc.mc_str(line))

    def chat(self, message: str) -> None:
        self.c.send("chat", mc.mc_str(message) + struct.pack(">qq", int(time.time() * 1000), 0) + b"\x00" + NOTHING_SEEN)

    def chat_visibility(self, visibility: int) -> None:
        """client_information as the options screen sends it: ChatVisiblity 0 full, 1 system, 2 hidden."""
        self.c.send("client_information", mc.mc_str("en_us") + struct.pack(">b", 10) + mc.varint(visibility) + b"\x01" +
                    b"\x7f" + mc.varint(1) + b"\x00\x01" + mc.varint(0))


SECTIONS = ("basics", "blocks", "give", "moving", "entities", "nbt", "signs", "tellraw", "sounds", "talking", "time",
            "permissions")


def record(port: int, out_path: str, name_a: str, name_b: str, console: str | None = None, only: set | None = None) -> None:
    """`only`: the sections to play (SECTIONS; a part of a name will do). The beginning - joining,
    the operator, the spot - is always played; a recording of some sections is compared with
    another of the same ones."""
    steps: list[dict] = []
    current = ["setup"]

    def section(name: str) -> None:
        assert name in SECTIONS
        current[0] = name

    def wanted() -> bool:
        return not only or current[0] == "setup" or any(word in current[0] for word in only)

    name_c = (name_a + "C")[:16]     # a third, who only comes by
    a, b = Prober(port, name_a), Prober(port, name_b)
    for bot in (a, b):
        bot.join()
        bot.pump(1.5)
        bot.c.send("player_loaded")
        bot.tick_end()
        bot.pump(0.5)


    gone = set()   # the bots that were thrown out (B, at the end)

    def pump(bot, seconds: float) -> None:
        if bot in gone:
            return
        try:
            bot.tick_end()
            bot.pump(seconds)
        except (ConnectionError, OSError):
            gone.add(bot)

    def type_on_console(line: str) -> None:
        if console:
            with open(console, "w") as f:
                f.write(line + "\n")
        else:
            print(f">>> type on the server's console now:  {line}")
        for _ in range(1 if console else 8):
            for bot in (a, b):
                pump(bot, 0.5)

    def step(name: str, action=None, ticks: int = 5):
        """Does `action` and takes what the bots are sent for it. A step lasts `ticks` ticks at
        most; one of the usual five is over as soon as something has come and two ticks have
        then passed without anything more - an answer is there in a tick or two, and waiting
        out the rest for each of four hundred lines is most of what a recording takes."""
        if not wanted():
            return
        t0 = time.time()
        mark_a, mark_b = len(a.log), len(b.log)
        seen, quiet = 0, 0
        for i in range(ticks):
            if i == 0 and action:
                action()
            for bot in (a, b):
                pump(bot, 0.001)
            time.sleep(max(0.0, t0 + (i + 1) * 0.05 - time.time()))
            now = len(a.log) - mark_a + len(b.log) - mark_b
            quiet = quiet + 1 if now == seen else 0
            seen = now
            if ticks == 5 and seen and quiet >= 2:
                break
        pump(a, 0.03)
        pump(b, 0.01)
        steps.append({"step": name, "A": a.log[mark_a:], "B": b.log[mark_b:]})
        print(f"{name}: A {len(a.log) - mark_a}, B {len(b.log) - mark_b} packets")

    # (Nothing spawns by itself while this records: a horse that walks by is a row of its own.
    # The rule is put back at the end; ours has no such rule and says so on its console.)
    type_on_console("gamerule minecraft:spawn_mobs false")
    type_on_console(f"op {name_a}")   # A is told its level and sent the commands that come with it

    def run(command: str, bot=a, ticks: int = 5):
        step(("" if bot is a else "B: ") + "/" + command, lambda: bot.say(command), ticks)

    # The tree as each was sent it: B on joining, A then and again as an operator.
    steps.append({"step": "joined", "A": [x for x in a.log if x[0] in ("commands", "entity_event")],
                  "B": [x for x in b.log if x[0] in ("commands", "entity_event")]})

    # A spot of its own for every pair of names, far from where anything else happened.
    spot = zlib.crc32((name_a + name_b).encode())
    ox, oz = 4000 + spot % 500 * 32, 4000 + spot // 500 % 500 * 32
    oy = -60    # on top of a flat world's grass
    step("A goes to its spot", lambda: a.say(f"tp {ox} {oy} {oz}"), ticks=30)
    step("...and fetches B", lambda: a.say(f"tp {name_b} {ox + 2} {oy} {oz}"), ticks=30)
    origin = (ox, oy, oz)

    section("basics")
    run("list")
    run("list uuids")
    run("list", b)
    run("help")
    run("help setblock")
    run("help tp")
    run("help teleport @s")
    run("help nonsense")

    # ---- blocks
    section("blocks")
    run("setblock ~ ~ ~2 stone")
    run("setblock ~ ~ ~2 stone")                      # nothing changes: failed
    run("setblock ~ ~ ~2 minecraft:glowstone keep")   # not air: failed
    run("setblock ~ ~ ~2 glowstone replace")
    run("setblock ~ ~ ~2 air destroy")                # drops the glowstone dust
    run("setblock ~ ~1 ~2 chest[facing=east]")
    run("setblock ~1 ~1 ~2 chest[facing=east,type=left] strict")
    run("setblock ~ ~1 ~2 air")
    run("setblock ~1 ~1 ~2 air")
    run("setblock ~ 5000 ~ stone")                    # out of the world
    run("setblock ~ ~ ~2 nonsense")
    run("setblock ~ ~ ~2 chest[facing=up]")
    run("setblock ~ ~ ~2 chest[colour=red]")
    run("setblock ~ ~ ~2 #logs")
    run("setblock ~ ~")
    run("setblock ~ ~ ~2")
    run("setblock ~ ~ ~2 stone please")
    run("fill ~-2 ~ ~3 ~2 ~2 ~5 stone")
    run("fill ~-2 ~ ~3 ~2 ~2 ~5 glowstone hollow")
    run("fill ~-2 ~ ~3 ~2 ~2 ~5 oak_planks outline")
    run("fill ~-2 ~ ~3 ~2 ~2 ~5 stone replace oak_planks")
    run("fill ~-2 ~ ~3 ~2 ~2 ~5 dirt replace #minecraft:planks")   # none left: failed
    run("fill ~-2 ~ ~3 ~2 ~2 ~6 cobblestone keep")
    run("fill ~-2 ~ ~3 ~2 ~2 ~6 air")
    run("fill ~-2 ~ ~3 ~2 ~2 ~6 air")                 # nothing changes: failed
    run("fill ~ ~ ~ ~100 ~100 ~100 stone")            # too many
    run("fill ~ ~ ~3 ~ ~ ~3 tnt strict")
    run("fill ~ ~ ~3 ~ ~ ~3 air destroy")             # the TNT comes out as an item

    # ---- items
    section("give")
    run("give @s diamond")
    run("give @s minecraft:stone 3")
    run(f"give {name_b} oak_planks 64")
    run("give @a stick 2")
    run("give @s ender_pearl 1601")                   # more than a hundred stacks of 16
    run("give @s stone 0")
    run("give @s nonsense")
    run("give Nobody stone")
    run("give @e stone")
    # ...with components: `item[type=value,!type]`, each value a tag read by the component's Codec
    run("give @s diamond_sword[damage=3]")
    run("give @s stone[custom_name=\"Named\",lore=[\"a\",{text:\"b\",color:\"red\"}]]")
    run("give @s diamond_sword[enchantments={sharpness:5},unbreakable={}] 1")
    run("give @s stone[rarity=epic,!lore]")
    run("give @s stone[item_name={translate:\"block.minecraft.dirt\"}]")
    run("give @s stone[max_stack_size=16] 20")
    run("give @s stone[ damage = 3 , !lore ]")
    run("give @s stone[!damage]")
    run("give @s stone[]")
    run("give @s written_book[written_book_content={title:\"T\",author:\"A\",pages:[\"one\",{text:\"two\",bold:true}]}]")
    run("give @s potion[potion_contents=\"swiftness\"]")
    run("give @s firework_rocket[fireworks={flight_duration:2b,explosions:[{shape:\"star\",colors:[I;255,65280]}]}]")
    run("give @s stone[custom_data={b:1,a:\"x\"}]")
    run("give @s potion[potion_contents={custom_effects:[{id:\"slowness\",amplifier:1b,duration:10,hidden_effect:{duration:500}}]}]")
    run("give @s stone[max_stack_size=64,rarity=common,!damage]")   # all as the item is: no components
    run("give @s shield[base_color=red]")
    # stacks in stacks, and the other components that are only data
    run("give @s shulker_box[container=[{slot:2,item:{id:\"stone\",count:5}},{slot:0,item:\"dirt\"}]]")
    run("give @s shulker_box[container=[{slot:0,item:{id:\"diamond_sword\",count:2}}]]")          # more than a stack of swords
    run("give @s shulker_box[container=[{slot:0,item:{id:\"stone\",count:65}}]]")
    run("give @s shulker_box[container=[{slot:0,item:{id:\"stone\",count:100}}]]")
    run("give @s shulker_box[container=[{slot:256,item:\"stone\"}]]")
    run("give @s bundle[bundle_contents=[\"stone\",{id:\"diamond_sword\",components:{damage:3,\"!lore\":{}}}]]")
    run("give @s bundle[bundle_contents=[{id:\"stone\",components:{creative_slot_lock:{}}}]]")   # no such component, in a tag
    run("give @s crossbow[charged_projectiles=[{id:\"arrow\"}]]")
    run("give @s decorated_pot[pot_decorations={front:\"brick\",back:{id:\"angler_pottery_sherd\"}}]")
    run("give @s bee_nest[bees=[{entity_data:{id:\"bee\",NoAI:1b},ticks_in_hive:5,min_ticks_in_hive:2400}]]")
    run("give @s stone[custom_model_data={floats:[1.5f,2],flags:[1b,0b],strings:[\"a\"],colors:[255]}]")
    run("give @s iron_chestplate[trim={material:\"gold\",pattern:\"coast\"}]")
    run("give @s compass[lodestone_tracker={target:{dimension:\"the_nether\",pos:[I;-5,70,1000]},tracked:0b}]")
    run("give @s stone[break_sound=\"item.shield.break\"]")
    run("give @s stone[break_sound={sound_id:\"probe:crack\",range:16}]")
    run("give @s stone[break_sound=\"no.such.sound\"]")
    run("give @s knowledge_book[recipes=[\"stick\",\"probe:nothing\"]]")
    run("give @s chest[container_loot={loot_table:\"chests/simple_dungeon\",seed:5}]")
    run("give @s filled_map[map_decorations={here:{type:\"red_x\",x:1,z:2.5d,rotation:90}}]")
    run("give @s debug_stick[debug_stick_state={oak_stairs:\"facing\"}]")
    run("give @s arrow[intangible_projectile={}]")
    run("give @s stone[wolf/variant=snowy,rabbit/variant=evil,horse/variant=dark_brown,villager/variant=taiga]")
    run("give @s stone[fox/variant=1]")
    run("give @s stone[provides_banner_patterns=\"#pattern_item/globe\"]")
    run("give @s stone[provides_banner_patterns=[globe,creeper]]")
    run("give @s stone[provides_banner_patterns=\"#no_such_tag\"]")
    run("give @s stone[creative_slot_lock={}]")
    run("give @s player_head[profile={id:[I;1,2,3,4],name:\"ProbeHead\",properties:[{name:\"textures\",value:\"abc\"}]}]")
    run("give @s player_head[profile={properties:{textures:[\"a\",\"b\"]},model:\"slim\"}]")
    run("give @s player_head[profile={name:\"has space\"}]")
    run("give @s compass[lodestone_tracker={}]")
    run("give @s chest[lock={items:\"#logs\",count:{min:1,max:1},components:{damage:3}}]")
    run("give @s chest[lock={count:{min:5,max:1}}]")
    run("give @s potion")
    run("give @s diamond_sword[max_stack_size=16]")   # what wears down cannot be stacked
    run("give @s stone[damage=3,damage=4]")
    run("give @s stone[nonsense=1]")
    run("give @s stone[creative_slot_lock={}]")       # a component that only travels
    run("give @s stone[damage=]")
    run("give @s stone[damage=-1]")
    run("give @s stone[rarity=legendary]")
    run("give @s stone[damage 3]")
    run("give @s stone[damage=3")
    run("give @s stone[damage=3,]")
    run("give @s stone[")
    run("give @s stone[!")
    run("give @s stone[damage=3]x")

    # ---- moving
    section("moving")
    run("tp ~ ~5 ~")
    run(f"tp @s {ox}.5 {oy} {oz}.5 90 45")
    run("tp @s ~ ~ ~ ~10 ~")
    run(f"tp {name_b}")
    run(f"teleport {name_b} {name_a}")
    run(f"tp @a {ox} {oy + 1} {oz}")
    run(f"tp {name_b} {ox + 2} {oy} {oz}")
    run(f"tp {ox} {oy} {oz}")
    run("tp @s 0 30000000 0")                         # outside the world
    run("tp ~ ~")
    run("tp Nobody")
    run("tp @e[type=nonsense]")
    run("tp @e[type=player,limit=0]")
    run("tp @x")
    run(f"tp @a[name={name_b},distance=..10] ~ ~ ~")

    # ---- entities
    section("entities")
    run("summon armor_stand ~ ~ ~3")
    run("summon minecraft:armor_stand ~1 ~ ~3")
    run("kill @e[type=armor_stand,distance=..10,limit=1,sort=nearest]")
    run("summon armor_stand ~2 ~ ~3")
    run("kill @e[type=armor_stand,distance=..10]")
    run("kill @e[type=armor_stand,distance=..10]")    # none left
    run("summon item ~ ~ ~3")
    run("summon player")
    run("summon nonsense")
    run("summon armor_stand 0 30000000 0")

    # ---- NBT: what `/data get` shows of a block entity, an entity, a storage; paths; tags as
    # text (SNBT) where a command takes one - right ones, and wrong ones for the error and its place
    section("nbt")
    # (What A carries by now depends on which of the dropped items flew its way: what it holds
    # from here on is put there.)
    step("A takes a diamond in hand, three stones beside it", lambda: (a.give(36, "kDiamond", 1), a.give(37, "kStone", 3), a.select(0)))
    run("setblock ~ ~ ~2 chest")
    run("data get block ~ ~ ~2")
    run("data get block ~ ~ ~2 Items")
    run("data get block ~ ~ ~2 x")
    run("data get block ~ ~ ~2 x 2.5")
    run("data get block ~ ~ ~2 y 0.125")
    run("data get block ~ ~ ~2 id")
    run("data get block ~ ~ ~2 Items[0]")             # nothing there
    run("data get block ~ ~ ~2 nope.deeper")
    run("data get block ~ ~ ~2 id 2")                 # not a number
    run("data get block ~ ~ ~2 {x:1}")                # the root does not match
    run("data get block ~ ~ ~2 {components:{}}.id")
    run("data get block ~ ~ ~3")                      # no block entity
    run("data get block ~ 5000 ~")
    run("setblock ~ ~ ~2 furnace")
    run("data get block ~ ~ ~2")
    run("data get block ~ ~ ~2 speed_multiplier 3")
    run("data get block ~ ~ ~2 RecipesUsed")
    run("setblock ~ ~ ~2 air")
    # A block entity out of a tag: what is wrong in it is left out (a slot that is none - 300 is
    # the byte 44 -, an item that is none, a hundred of something, an entry no chest has).
    chest = ('chest{CustomName:"Loot",lock:{items:"stick"},foo:1b,components:{rarity:"rare",nope:1},Items:['
             '{Slot:3b,id:"stone",count:5},{Slot:300,id:"dirt"},{Slot:1b,id:"diamond_sword",components:{damage:7}},'
             '{Slot:2b,id:"nope"},{Slot:4b,id:"apple",count:100},{id:"dirt",count:2}]}')
    run("setblock ~ ~ ~2 " + chest)
    run("data get block ~ ~ ~2")
    run("data get block ~ ~ ~2 Items")
    run("setblock ~ ~ ~2 " + chest)                   # the same again: nothing changed

    def click_chest() -> None:
        a.c.send("use_item_on", mc.varint(0) + mc.pack_block_pos(ox, oy, oz + 2) + mc.varint(1) + struct.pack(">fff", 0.5, 1.0, 0.5) +
                 b"\x00\x00" + mc.varint(900))
    step("A clicks the chest, a diamond in hand: it is locked", click_chest)
    step("A clicks it with a stick: its name on the menu", lambda: (a.give(36, "kStick", 1), click_chest()), ticks=8)
    step("A closes it and takes the diamond again", lambda: (a.c.send("container_close", mc.varint(1)), a.give(36, "kDiamond", 1)))
    run("setblock ~ ~ ~2 chest{CustomName:{text:\"J\",color:\"red\"},lock:{}}")
    run("data get block ~ ~ ~2")
    run("setblock ~ ~ ~2 chest{CustomName:5,lock:{count:{min:5,max:1}},components:\"x\",Items:{}}")   # all of it wrong
    run("data get block ~ ~ ~2")
    run("setblock ~ ~ ~2 furnace{lit_total_time:1600s,cooking_total_time:\"x\",speed_multiplier:2,"
        "RecipesUsed:{\"minecraft:iron_ingot\":3},Items:[{Slot:2b,id:\"iron_ingot\",count:3}],CustomName:\"Hot\"}")
    run("data get block ~ ~ ~2")
    run("setblock ~ ~ ~2 stone{a:1}")                 # no block entity: the tag is nobody's
    run("setblock ~ ~ ~2 chest{")
    run("setblock ~ ~ ~2 chest{a:}")
    run("fill ~ ~ ~2 ~2 ~ ~2 barrel{Items:[{Slot:0b,id:\"apple\"}],CustomName:\"B\"}")
    run("data get block ~1 ~ ~2")
    run("setblock ~1 ~ ~2 barrel")                    # the barrel it was, with what it held: nothing changed
    run("setblock ~1 ~ ~2 barrel[facing=up]")         # another state of the same block: it keeps its block entity
    run("data get block ~1 ~ ~2 Items")
    run("setblock ~1 ~ ~2 air")
    run("setblock ~1 ~ ~2 barrel")
    run("fill ~ ~ ~2 ~2 ~ ~2 stone replace barrel{Items:[{id:\"minecraft:apple\"}]}")   # two of the three
    run("fill ~ ~ ~2 ~2 ~ ~2 dirt replace #minecraft:planks{a:1}")                       # no block entity among them
    run("fill ~ ~ ~2 ~2 ~ ~2 air")
    run("data get entity @s Pos")
    run("data get entity @s Pos[0] 2")
    run("data get entity @s Pos[-1]")
    run("data get entity @s Pos[]")                   # more than one
    run("data get entity @s Pos[3]")
    run("data get entity @s Rotation")
    run("data get entity @s OnGround")
    run("data get entity @s UUID")
    run("data get entity @s UUID[1]")
    run("data get entity @s SelectedItem")
    run("data get entity @s SelectedItem.id")
    run("data get entity @s SelectedItem.count 0.5")
    run("data get entity @s Inventory[0]")
    run("data get entity @s Inventory[{Slot:1b}].id")
    run("data get entity @s {OnGround:1b}.Air")
    run("data get entity @s foo.bar")
    run("data get entity @a")
    run("data get entity Nobody")
    run(f"data get entity {name_b} Pos")
    run("summon armor_stand ~ ~ ~3")
    run("data get entity @e[type=armor_stand,distance=..10,limit=1]")          # whole: compared by its keys
    run("data get entity @e[type=armor_stand,distance=..10,limit=1] Pos")
    run("kill @e[type=armor_stand,distance=..10]")
    run("summon pig ~ ~ ~3")
    run("data get entity @e[type=pig,distance=..10,limit=1]")
    run("kill @e[type=pig,distance=..10]")
    run("data get entity @s")
    run("summon tnt ~ ~ ~3")                          # (its fuse is a matter of when one asks)
    run("data get entity @e[type=tnt,distance=..10,limit=1] block_state")
    run("kill @e[type=tnt,distance=..10]")
    run("summon falling_block ~ ~40 ~3")              # sand, on its way down for two seconds
    run("data get entity @e[type=falling_block,distance=..50,limit=1] BlockState")
    run("data get entity @e[type=falling_block,distance=..50,limit=1] DropItem")
    run("data get entity @e[type=falling_block,distance=..50,limit=1] FallHurtMax 0.5")
    run("kill @e[type=falling_block,distance=..50]")
    run("data get storage probe:nothing")
    run("data get storage probe:nothing a")
    run("data get storage nothing")
    run("data get storage Bad:Id")
    run("data get storage probe:nothing a..b")
    run("data get storage probe:nothing a[")
    run("data get storage probe:nothing a[x]")
    run("data get storage probe:nothing a[1")
    run("data get storage probe:nothing a.{b:1}")
    run("data get storage probe:nothing a{b:1}.c")
    run("data get storage probe:nothing \"a key\".'b'[{c:1}][0][]")
    run("data get storage probe:nothing {a:1")
    run("data get storage probe:nothing {} 1")
    run("data get")
    run("data get block")
    run("tp @s[nbt={OnGround:1b}] ~ ~ ~")
    run("tp @s[nbt={OnGround:0b}] ~ ~ ~")             # nobody
    run("tp @s[nbt=!{OnGround:0b}] ~ ~ ~")
    run("tp @s[nbt={Pos:[]}] ~ ~ ~")                  # an empty list asks for an empty one
    run(f"tp @s[nbt={{Pos:[{oy}.0d]}}] ~ ~ ~")        # a list asks for its elements, anywhere
    run("tp @s[nbt={SelectedItem:{id:\"minecraft:diamond\"}}] ~ ~ ~")
    run("tp @a[nbt={},limit=1] ~ ~ ~")
    run("tp @s[nbt=5] ~ ~ ~")
    run("tp @s[nbt={a:}] ~ ~ ~")
    # ---- signs: where one stands, hangs on a wall or from a ceiling; its editor; dye, ink, wax;
    # what holds it; one out of a tag. A looks south (the yaw it is put at), from the block's middle.
    section("signs")
    run("tp @s ~ ~ ~ 0 0")
    use_seq = [1000]

    def use(dx: int, dy: int, dz: int, face: int) -> None:
        use_seq[0] += 1
        a.c.send("use_item_on", mc.varint(0) + mc.pack_block_pos(ox + dx, oy + dy, oz + dz) + mc.varint(face) +
                 struct.pack(">fff", 0.5, 0.5, 0.5) + b"\x00\x00" + mc.varint(use_seq[0]))

    def write(dx: int, dy: int, dz: int, lines: list, front: bool = True) -> None:
        a.c.send("sign_update", mc.pack_block_pos(ox + dx, oy + dy, oz + dz) + b"".join(mc.mc_str(line) for line in lines) +
                 mc.varint(1 if front else 0))

    step("A places a sign on the ground, two blocks ahead", lambda: (a.give(36, "kOakSign", 8), use(0, -1, 2, 1)), ticks=8)
    step("A writes on it", lambda: write(0, 0, 2, ["Hello", "\u00a7cred?", "", "x" * 40]))
    step("A writes on it again: nobody may", lambda: write(0, 0, 2, ["again", "", "", ""]))
    run("data get block ~ ~ ~2")
    step("A clicks it with an empty hand: the editor", lambda: (a.give(36, None), use(0, 0, 2, 2)), ticks=8)
    step("A writes two lines", lambda: write(0, 0, 2, ["Hello", "world", "", ""]))
    step("A dyes it red", lambda: (a.give(36, "kRedDye", 1), use(0, 0, 2, 2)), ticks=8)
    step("A dyes it red again: the editor instead", lambda: use(0, 0, 2, 2), ticks=8)
    step("A makes it glow", lambda: (a.give(36, "kGlowInkSac", 1), use(0, 0, 2, 2)), ticks=8)
    step("A takes the glow off", lambda: (a.give(36, "kInkSac", 1), use(0, 0, 2, 2)), ticks=8)
    step("A waxes it", lambda: (a.give(36, "kHoneycomb", 1), use(0, 0, 2, 2)), ticks=8)
    step("A clicks the waxed sign with an empty hand", lambda: (a.give(36, None), use(0, 0, 2, 2)), ticks=8)
    step("A dyes the waxed sign: nothing", lambda: (a.give(36, "kBlueDye", 1), use(0, 0, 2, 2)), ticks=8)
    run("data get block ~ ~ ~2")
    run("setblock ~1 ~ ~1 stone")
    step("A places a sign against the stone's west side", lambda: (a.give(36, "kOakSign", 8), use(1, 0, 1, 4)), ticks=8)
    run("setblock ~ ~2 ~1 stone")
    step("A hangs a sign under a block", lambda: (a.give(36, "kOakHangingSign", 8), use(0, 2, 1, 0)), ticks=8)
    run("setblock ~1 ~ ~1 air")                      # what the wall sign hung on
    run("setblock ~ ~2 ~1 air")                      # ...and what the hanging one hung from
    run("setblock ~ ~-1 ~2 air")                     # ...and what the first one stood on
    run("setblock ~ ~-1 ~2 grass_block")
    run("setblock ~ ~ ~2 oak_sign[rotation=4]{front_text:{messages:[\"a\",{text:\"b\",color:\"red\"},\"\",\"\"],color:\"blue\"},"
        "is_waxed:1b,back_text:{messages:[\"only three\",\"\",\"\"]},foo:1}")
    run("data get block ~ ~ ~2")
    run("setblock ~ ~ ~2 air")
    run("setblock ~ ~ ~2 oak_sign{front_text:{messages:[{text:\"x\",click_event:{action:\"run_command\",command:\"say hi\"}},"
        "\"\",\"\",\"\"]}}")
    step("A clicks a sign with a command on it: not allowed", lambda: (a.give(36, None), use(0, 0, 2, 2)), ticks=8)
    run("setblock ~ ~ ~2 air")
    run("setblock ~ ~ ~2 oak_wall_sign")             # nothing behind it: it is set all the same
    run("setblock ~ ~ ~2 air")
    step("A takes its diamond again", lambda: a.give(36, "kDiamond", 1))

    section("tellraw")
    run("tellraw @s \"quoted\"")
    run("tellraw @s 'single'")
    run("tellraw @s bare")
    run("tellraw @a {text:\"to all\",color:\"red\"}")
    run("tellraw @s [\"a\",{text:\"b\",bold:true},{text:'c',italic:1b}]")
    run("tellraw @s {translate:\"chat.type.text\",with:[\"x\",{text:\"y\",color:\"#12AB34\"}]}")
    run("tellraw @s {text:\"\\u0041\\x42\\U00000043\\s\\\"\\'\\\\\"}")
    run("tellraw @s {text : 'spaced' , }")
    run("tellraw @s {\"text\":\"json too\"}")
    run(f"tellraw {name_b} \"for B\"")
    run("tellraw Nobody \"x\"")
    run("tellraw @s")
    for wrong in ("{text:\"a\"", "\"abc", "'abc", "\"a\\qb\"", "\"\\u12\"", "\"\\U00110000\"", "\"\\N{nope}\"",
                  "[300b]", "[70000s]", "[2147483648]", "[9223372036854775808L]", "[0x1FFFFFFFF]", "[0x1FFFFFFFFi]",
                  "[256ub]", "[65536us]", "[4294967296ui]", "[18446744073709551616uL]", "[-1ub]", "[-0x1i]",
                  "[0x100000000si]", "[1__0]", "[_1]", "[1_]", "[0x]", "[0b]", "[0b2]", "007", "[1e999]", "[1e999f]",
                  "[.]", "-", "+", "{\"\":\"x\"}", "{a:1,}x", "{a:}", "{:1}", "{a 1}", "[1,,2]", "[B;1s]", "[I;1L]",
                  "[L;a]", "[B;1.5]", "bool(a)", "uuid(zz)", "uuid(1)", "nope(1,2)", "bool()", "{a:1}}", "[1 2]",
                  "{a:[1,{b:\"c}]}"):
        run("tellraw @s " + wrong)

    # ---- sounds
    section("sounds")
    run("playsound minecraft:block.note_block.harp")
    run("playsound block.note_block.harp block @a ~ ~ ~ 0.5 2")
    run(f"playsound some:thing voice {name_b} ~ ~500 ~")            # too far
    run(f"playsound some:thing voice {name_b} ~ ~500 ~ 1 1 0.25")   # ...but with a least volume
    run("playsound block.note_block.harp master @a ~ ~ ~ 3")
    run("playsound block.note_block.harp master @a ~ ~ ~ 1 5")
    run("stopsound @a")
    run("stopsound @a music")
    run("stopsound @a * minecraft:block.note_block.harp")
    run(f"stopsound {name_b} block block.note_block.harp")

    # ---- talking: what is typed into the chat, and the commands that say something
    section("talking")
    def says(text: str, bot=a):
        step(("" if bot is a else "B ") + "says: " + text[:40], lambda: bot.chat(text))

    def signed(command: str, bot=a):
        step(("" if bot is a else "B: ") + "/" + command[:60] + " (as a client sends it)", lambda: bot.say_signed(command))

    def on_console(line: str):
        if not wanted():
            return
        mark_a, mark_b = len(a.log), len(b.log)
        type_on_console(line)
        steps.append({"step": "console: " + line, "A": a.log[mark_a:], "B": b.log[mark_b:]})

    says("hello")
    says("hello back", b)
    says("x" * 256)
    says("a \u00e4 \u4e16 \U0001F600 z")
    signed("me waves")
    run("me waves")                                   # a plain chat_command: said for A, by nobody
    signed("me waves at @a", b)                       # B may not use selectors
    signed("say hello @s and @a")
    run("say hello @s and @a")
    signed("say a@b @ @x @")
    signed("say hi @e[type=nothing]")
    signed("say hi @e[type=armor_stand,distance=..2]")   # nobody: an empty name
    signed("say " + "x" * 257)
    signed("say " + "x" * 256)
    run("say hi", b)                                  # not a command to B
    signed(f"msg {name_b} psst")
    signed(f"tell {name_a} what", b)
    signed("w @a all of you")
    run(f"msg {name_b} not signed")
    signed("msg Nobody hi")
    signed("msg @s")
    signed("teammsg hi")
    signed("tm hi", b)
    on_console("say console here, @a")
    on_console("me yawns")
    on_console(f"msg {name_b} you there?")
    on_console("teammsg hi")
    # B shows system messages only, then nothing, then everything again.
    step("B: system messages only", lambda: b.chat_visibility(1))
    says("can B read this?")
    says("B says this", b)
    run("list", b)
    step("B: chat hidden", lambda: b.chat_visibility(2))
    says("and this?")
    says("B says that", b)
    run("list", b)
    signed(f"msg {name_b} hidden?")
    step("B: chat shown", lambda: b.chat_visibility(0))
    says("B is back")
    # Somebody comes and goes: what the others read, and what it reads itself.
    if wanted():
        mark_a, mark_b = len(a.log), len(b.log)
        c = Prober(port, name_c)
        c.join()
        c.pump(1.0)
        for bot in (a, b):
            pump(bot, 0.3)
        c.chat("just passing")
        c.pump(0.5)
        c.c.close()
        for _ in range(4):
            for bot in (a, b):
                pump(bot, 0.25)
        steps.append({"step": "C joins, says something and leaves", "A": a.log[mark_a:], "B": b.log[mark_b:], "C": c.log})

    section("time")
    # ---- the time: the world's clocks. The overworld's is stopped first, so that it is where
    # each command puts it; the end's runs on.
    run("time pause")
    run("time pause")
    run("time set 1000")
    run("time set 1000")
    run("time set 50s")
    run("time set day")                               # it is that time already
    run("time set noon")
    run("time set minecraft:night")
    run("time set day")                               # tomorrow's
    run("time set teatime")
    run("time set -5")                                # no time: a time marker of that name
    run("time set 12:30")
    run("time set 1x")
    run("time set -5D")
    run("time set")
    run("time")
    run("time add 2s")
    run("time add -1d")
    run("time add -100000")
    run("time add 0.5")
    run("time set 3.5d")
    run("time add 6000")
    run("time query time")
    run("time query day")
    run("time query day repetition")
    run("time query minecraft:moon")
    run("time query early_game repetition")
    run("time query villager_schedule")
    run("time query week")
    run("time query gametime")
    run("time of the_end query day")
    run("time of the_end set noon")
    run("time of minecraft:overworld query time")
    run("time of nowhere pause")
    run("time of the_end query gametime")
    run("time rate 2.5")
    run("time rate 2.5")
    run("time rate 1001")
    run("time rate 0")
    run("time rate 1")
    run("time set day", b)                            # not a command to B
    for line in ("/time set ", "/time set n", "/time set mid", "/time set MID", "/time set minecraft:", "/time set minecraft:noon",
                 "/time set 5", "/time set 0", "/time set 1.5d", "/time set x", "/time add 7", "/time query ", "/time query d",
                 "/time query t", "/time of the_end set ", "/time of the_end query ", "/time of overworld set n", "/time ",
                 "/time q", "/time set", "time set n"):
        step("A asks what could follow: " + line, lambda line=line: a.suggest(line), ticks=3)
    step("B asks what could follow: /time set ", lambda: b.suggest("/time set "), ticks=3)
    step("A asks twice in one tick", lambda: (a.suggest("/time set d"), a.suggest("/time set m")), ticks=3)
    run("time resume")                                # as it was: running, at its own rate
    run("time resume")

    # ---- what is not a command, and who may run what
    section("permissions")
    run("nonsense")
    run("")
    run("list all of them")
    run("setblock ~ ~ ~2 stone", b)
    run("help", b)
    run("help setblock", b)
    run(f"op {name_b}")
    run(f"op {name_b}")                               # already one
    run("setblock ~ ~ ~4 stone", b)
    run("setblock ~ ~ ~4 air", b)
    run(f"deop {name_b}")
    run(f"deop {name_b}")
    run("kick Nobody")
    run(f"kick {name_b} that is enough")

    type_on_console(f"deop {name_a}")
    type_on_console("gamerule minecraft:spawn_mobs true")
    a.c.close()
    Path(out_path).write_text(json.dumps({"origin": origin, "names": [name_a, name_b, name_c], "recorded": int(time.time() * 1000), "steps": steps}))
    print(f"wrote {out_path}")


# ----------------------------------------------------------------------------- the command tree

def _argument_names() -> dict[int, str]:
    """minecraft:command_argument_type by id, from Mojang's registries report."""
    report = sorted((ROOT / ".cache" / "mc").glob("*/reports/registries.json"))
    if not report:
        sys.exit("command_probe: reports/registries.json is missing (mcgen.py reports)")
    entries = json.loads(report[-1].read_text())["minecraft:command_argument_type"]["entries"]
    return {e["protocol_id"]: name for name, e in entries.items()}


def parse_tree(r: mc.Reader) -> dict:
    """ClientboundCommandsPacket with every argument type of vanilla (the test client's
    parse_commands reads the ones this server names). The properties per type are those of the
    ArgumentTypeInfo classes in net.minecraft.commands.synchronization and .arguments."""
    names = _argument_names()
    nodes = []
    for _ in range(r.varint()):
        flags = r.u8()
        node = {"type": ("root", "literal", "argument")[flags & 3], "executable": bool(flags & 4),
                "restricted": bool(flags & 32), "child_ids": [r.varint() for _ in range(r.varint())]}
        if flags & 8:
            node["redirect_id"] = r.varint()
        if flags & 3:
            node["name"] = r.str()
        if flags & 3 == 2:
            parser = names[r.varint()]
            props = {}
            if parser in ("brigadier:float", "brigadier:double", "brigadier:integer", "brigadier:long"):
                bounds = r.u8()
                read = {"brigadier:float": r.f32, "brigadier:double": r.f64, "brigadier:integer": r.i32, "brigadier:long": r.i64}[parser]
                if bounds & 1:
                    props["min"] = read()
                if bounds & 2:
                    props["max"] = read()
            elif parser == "brigadier:string":
                props["type"] = ("word", "phrase", "greedy")[r.varint()]
            elif parser == "minecraft:entity":
                mask = r.u8()
                props = {"amount": "single" if mask & 1 else "multiple", "type": "players" if mask & 2 else "entities"}
            elif parser == "minecraft:score_holder":
                props["amount"] = "multiple" if r.u8() & 1 else "single"
            elif parser == "minecraft:time":
                props["min"] = r.i32()
            elif parser in ("minecraft:resource_or_tag", "minecraft:resource_or_tag_key", "minecraft:resource",
                            "minecraft:resource_key", "minecraft:resource_selector"):
                props["registry"] = r.str()
            node["parser"], node["properties"] = parser, props
            if flags & 16:
                node["suggestions"] = r.str()
        nodes.append(node)
    root = nodes[r.varint()]
    r.done()
    for node in nodes:
        node["children"] = {nodes[i]["name"]: nodes[i] for i in node.pop("child_ids")}
        if "redirect_id" in node:
            node["redirect"] = nodes[node.pop("redirect_id")].get("name", "")   # "" is the root (execute run)
    return root


def _same_node(ours: dict, ref: dict, path: str, problems: list, extra: list) -> None:
    for key in ("type", "executable", "restricted", "parser", "properties", "suggestions", "redirect"):
        if ours.get(key) != ref.get(key):
            problems.append(f"{path}: {key} is {ours.get(key)!r}, vanilla's {ref.get(key)!r}")
    common = [n for n in ours["children"] if n in ref["children"]]
    if common != [n for n in ref["children"] if n in ours["children"]]:
        problems.append(f"{path}: children in the order {common}, vanilla's {[n for n in ref['children'] if n in ours['children']]}")
    for name, child in ours["children"].items():
        if name not in ref["children"]:
            problems.append(f"{path} {name}: not in vanilla's tree")
        else:
            _same_node(child, ref["children"][name], f"{path} {name}", problems, extra)
    extra.extend(f"{path} {name}" for name in ref["children"] if name not in ours["children"])


def compare_trees(ours: dict, ref: dict) -> tuple[list, list]:
    problems, extra = [], []
    for name, node in ours["children"].items():
        if name not in ref["children"]:
            problems.append(f"/{name}: vanilla has no such command")
        else:
            _same_node(node, ref["children"][name], "/" + name, problems, extra)
    return problems, extra


# ----------------------------------------------------------------------------- diff

def _text(v, bots: set):
    """A text component with the UUIDs that are random by nature replaced."""
    if isinstance(v, dict):
        out = {}
        for k, x in v.items():
            if k == "uuid" and isinstance(x, tuple) and len(x) == 4:
                hi, lo = (x[0] & 0xFFFFFFFF) << 32 | x[1] & 0xFFFFFFFF, (x[2] & 0xFFFFFFFF) << 32 | x[3] & 0xFFFFFFFF
                out[k] = x if f"{hi:016x}{lo:016x}" in bots else "<uuid>"
            elif k == "insertion" and isinstance(x, str) and UUID_RX.match(x) and x.replace("-", "") not in bots:
                out[k] = "<uuid>"
            else:
                out[k] = _text(x, bots)
        return out
    if isinstance(v, list):
        return [_text(x, bots) for x in v]
    return v


def _chat_type(r, bots: set) -> str:
    """ChatType.Bound: the type's id, the sender's name, the target's."""
    holder = r.varint()
    assert holder > 0, "a chat type spelled out in the packet"
    name = mc.snbt(_text(r.nbt(), bots))
    target = mc.snbt(_text(r.nbt(), bots)) if r.bool() else None
    return f"type {mc.load_registry('chat_type')[holder - 1]} name {name} target {target}"


def rec_time(st: dict, rec: dict) -> int:
    """About when a recording was made, in ms: a message's time is its server's, and "now"."""
    return rec.get("recorded", 0)


def _plain(v) -> str:
    """Enough of Component.getString to recognise a line: literal text and the children."""
    if isinstance(v, str):
        return v
    if isinstance(v, dict):
        return str(v.get("text", "")) + "".join(_plain(x) for x in v.get("extra", []))
    return ""


# Commands of which this server has some of the ways to write them: their line of `help` is another.
PARTLY = {"data"}   # `data get` alone: no merge, modify, remove

# What an entity saves, of the entries this server writes too (server/saved_tags.cpp) - but for
# those that are chance on vanilla (a mob is left-handed one time in twenty, its attributes come
# in the order of a map of objects, with a bonus drawn at random) or each server's own (the UUID
# of what was summoned).
ENTITY_KEYS = {"Pos", "Motion", "Rotation", "fall_distance", "Fire", "Air", "OnGround", "Invulnerable", "PortalCooldown",
               "Health", "HurtTime", "DeathTime", "AbsorptionAmount", "current_impulse_context_reset_grace_time", "FallFlying",
               "Brain", "CanPickUpLoot", "PersistenceRequired", "Age", "ForcedAge", "AgeLocked", "Invisible", "Small",
               "ShowArms", "DisabledSlots", "NoBasePlate", "Pose", "Inventory", "SelectedItemSlot", "abilities",
               "playerGameType", "Dimension", "SelectedItem", "equipment", "NoGravity"}


def _entries(shown: str) -> dict[str, str]:
    """The entries of a compound as `/data get` shows it: "{a: 1b, b: [x, y], c: {d: "e, f"}}"."""
    assert shown.startswith("{") and shown.endswith("}"), shown
    out, depth, quote, start, body = {}, 0, None, 0, shown[1:-1]
    parts = []
    for i, ch in enumerate(body):
        if quote:
            if ch == quote and body[i - 1] != "\\":
                quote = None
        elif ch in "\"'":
            quote = ch
        elif ch in "{[":
            depth += 1
        elif ch in "}]":
            depth -= 1
        elif ch == "," and depth == 0:
            parts.append(body[start:i])
            start = i + 1
    if body[start:].strip():
        parts.append(body[start:])
    for part in parts:
        k, _, v = part.strip().partition(": ")
        out[k] = v
    return out


def _normalise(rec: dict, who: str, commands: set | None) -> tuple[dict[str, list], dict]:
    origin = rec["origin"]
    bots = {mc.offline_uuid(n) for n in rec["names"]}
    out, tree = {}, None
    own_id = None
    for st in rec["steps"]:
        rows, appeared = [], []
        blocks = {}
        for name, hexdata in st.get(who, []):
            r = mc.Reader(bytes.fromhex(hexdata))
            if name == "player_chat":
                # The count of player_chat packets this client was sent (compared: both servers
                # count from 0), the sender, and a message without a session: index 0, no
                # signature, the server's own time, salt 0, nothing seen.
                count, sender, index = r.varint(), r.uuid(), r.varint()
                signature = r.take(256).hex() if r.bool() else None
                content, stamp, salt = r.str(), r.i64(), r.i64()
                seen = [r.take(256).hex() if (packed := r.varint()) == 0 else packed - 1 for _ in range(r.varint())]
                unsigned = _text(r.nbt(), bots) if r.bool() else None
                mask = r.varint()
                rows.append((name, f"#{count} from {sender} index {index} signature {signature} {content!r} "
                                   f"time {'now' if abs(stamp - rec_time(st, rec)) < 600_000 else stamp} salt {salt} seen {seen} "
                                   f"unsigned {mc.snbt(unsigned) if unsigned is not None else None} mask {mask} {_chat_type(r, bots)}"))
                r.done()
            elif name == "disguised_chat":
                rows.append((name, f"{mc.snbt(_text(r.nbt(), bots))} {_chat_type(r, bots)}"))
                r.done()
            elif name == "set_time":
                # The game time is each server's own. The beat of every twentieth tick names no
                # clock and falls where it falls; a clock that is named is compared in full, in
                # the order of its id (the map is a hash map's).
                r.i64()
                clocks = {}
                for _ in range(r.varint()):
                    clock = r.varint()
                    clocks[clock] = (r.varlong(), r.f32(), r.f32())
                r.done()
                if clocks:
                    # Where a clock that runs has come to is its world's own: the end's, always;
                    # the overworld's until it is stopped, and for whoever joins before that.
                    names = mc.load_registry("world_clock")
                    running = st["step"] in ("/time pause", "C joins, says something and leaves")
                    rows.append((name, " ".join(
                        f"{names[c]}: {'its own' if running or names[c] != 'minecraft:overworld' else t} ticks + {p} at rate {rate}"
                        for c, (t, p, rate) in sorted(clocks.items()))))
                else:
                    rows.append(("  (not compared)", "the game time"))
            elif name == "command_suggestions":
                asked, start, length = r.varint(), r.varint(), r.varint()
                texts = []
                for _ in range(r.varint()):
                    texts.append(r.str() + (" (" + mc.snbt(r.nbt()) + ")" if r.bool() else ""))
                r.done()
                rows.append((name, f"#{asked} {start}+{length}: {texts}"))
            elif name == "system_chat":
                value = _text(r.nbt(), bots)
                overlay = r.bool()
                line = _plain(value)
                key = value.get("translate", "") if isinstance(value, dict) else ""
                if key.startswith("chat.type.advancement"):
                    rows.append(("  (not compared)", key))
                    continue
                if st["step"].endswith("/help") or st["step"].endswith("B: /help"):   # one line per command: only those both have
                    if commands is not None and line.split(" ")[0].lstrip("/") not in commands - PARTLY:
                        continue
                if key == "commands.time.query.gametime" or (key == "commands.time.query.absolute" and "the_end" in line):
                    rows.append((name, key + " (a count of ticks: each server's own)"))
                    continue
                failed = value["extra"][0] if isinstance(value, dict) and value.get("color") == "red" and value.get("extra") else None
                if isinstance(failed, dict) and failed.get("translate") == "argument.component.invalid":
                    # Why it is none: DataFixerUpper's words on vanilla, this server's on ours.
                    rows.append((name, "argument.component.invalid (the reason is each server's own)"))
                    continue
                if isinstance(failed, dict) and failed.get("translate") == "arguments.item.component.malformed":
                    rows.append((name, f"arguments.item.component.malformed {failed['with'][0]} (the reason is each server's own)"))
                    continue
                if key == "commands.data.entity.query" and re.fullmatch(r"/data get entity \S+", st["step"]):
                    # An entity as a whole: the entries this server writes are compared, the others listed.
                    # (A player's Motion: vanilla's stands under one tick of gravity, ours is not moved by the server.)
                    # ...and what it carries depends on which of the dropped items flew its way.)
                    compared = ENTITY_KEYS - ({"Motion", "Inventory"} if st["step"].endswith(" @s") else set())
                    entries = _entries(_plain(value["with"][-1]))
                    rows.append((name, key + " " + ", ".join(f"{k}: {v}" for k, v in sorted(entries.items()) if k in compared)))
                    rows.append(("  (not compared)", "entries " + " ".join(sorted(k for k in entries if k not in compared))))
                    continue
                rows.append((name, mc.snbt(value) + (" (overlay)" if overlay else "")))
            elif name == "disconnect":
                rows.append((name, mc.snbt(_text(r.nbt(), bots))))
            elif name in ("block_update", "section_blocks_update"):
                changed = dict([mc.parse_block_update(r)]) if name == "block_update" else mc.parse_section_blocks_update(r)
                for pos, state in changed.items():
                    rel = tuple(p - o for p, o in zip(pos, origin))
                    # The ground: a random tick's doing. (C stands where it joined, on ground of another height.)
                    if (rel[1] == -1 or who == "C") and state in (mc.BLOCK["kDirt"], mc.BLOCK["kGrassBlock"]):
                        rows.append(("  (not compared)", f"grass and dirt at {rel}"))
                    else:
                        blocks[rel] = state
            elif name == "player_position":
                r.varint()
                pos, delta = (r.f64(), r.f64(), r.f64()), (r.f64(), r.f64(), r.f64())
                rot, flags = (r.f32(), r.f32()), r.i32()
                if who == "C":      # where one joins: vanilla picks a spot around the world's spawn, ours the spawn
                    rows.append(("  (not compared)", f"C joins at {pos}"))
                    continue
                rows.append((name, f"pos {pos} delta {delta} rot {rot} relative {flags:#x}"))
            elif name == "sound":
                holder = r.varint()
                if holder != 0:
                    if holder - 1 in SIGN_SOUNDS:
                        sound, pitch_free = SIGN_SOUNDS[holder - 1], False
                    elif holder - 1 != mc.SOUND["kEntityItemPickup"]:
                        continue     # a sound of the world (footsteps, a block placed), not of a command
                    else:
                        sound, pitch_free = "entity.item.pickup", True
                else:
                    sound, pitch_free = r.str(), False
                    assert not r.bool()
                source = r.varint()
                pos = tuple(v / 8 - o for v, o in zip((r.i32(), r.i32(), r.i32()), origin))
                volume, pitch = r.f32(), r.f32()
                rows.append((name, f"{sound} source {source} at {pos} volume {volume} pitch {'random' if pitch_free else pitch}"))
            elif name == "open_screen":
                r.varint()
                rows.append((name, f"menu {r.varint()} title {mc.snbt(r.nbt())}"))
            elif name == "block_entity_data":
                pos = mc.unpack_block_pos(r.u64())
                rows.append((name, f"at {tuple(p - o for p, o in zip(pos, origin))} type {r.varint()} {mc.snbt(r.nbt())}"))
            elif name == "open_sign_editor":
                pos = mc.unpack_block_pos(r.u64())
                rows.append((name, f"at {tuple(p - o for p, o in zip(pos, origin))} side {r.varint()}"))
            elif name == "level_event":
                kind = r.i32()
                pos = mc.unpack_block_pos(r.u64())
                if kind != 3003:
                    continue     # (a block that breaks is told by its block update here)
                rows.append((name, f"{kind} at {tuple(p - o for p, o in zip(pos, origin))} data {r.i32()}"))
            elif name == "stop_sound":
                flags = r.u8()
                rows.append((name, f"source {r.varint() if flags & 1 else '*'} sound {r.str() if flags & 2 else '*'}"))
            elif name == "entity_event":
                eid, event = r.i32(), r.u8()
                if 24 <= event <= 28:
                    own_id = eid if own_id is None else own_id
                    if eid == own_id:
                        rows.append((name, f"permission level {event - 24}"))
            elif name == "add_entity":
                r.varint()
                r.take(16)
                etype = r.varint()
                pos = tuple(round(v - o, 3) for v, o in zip((r.f64(), r.f64(), r.f64()), origin))
                if etype == mc.ENTITY_TYPE["kArmorStand"]:
                    appeared.append((name, f"armor stand at {pos}"))
            elif name == "commands":
                tree = parse_tree(r)
                rows.append(("  (not compared)", f"a command tree: {sorted(tree['children'])}"))
        if blocks:
            rows.append(("blocks", str(sorted(blocks.items()))))
        key, again = st["step"], 1
        while key in out:            # a step that is taken more than once is told apart by its turn
            again += 1
            key = f'{st["step"]} ({again})'
        out[key] = rows + appeared
    return out, tree


def diff(path_ref: str, path_ours: str) -> int:
    ref, ours = json.load(open(path_ref)), json.load(open(path_ours))
    different = 0
    verbose = "-v" in sys.argv
    for who in ("A", "B", "C"):
        right, tree_ours = _normalise(ours, who, None)
        commands = set(tree_ours["children"]) if tree_ours else set()
        left, tree_ref = _normalise(ref, who, commands)
        right, _ = _normalise(ours, who, commands)
        print(f"################ what {who} is told (left: {path_ref}, right: {path_ours})")
        for step in left:
            x, y = left[step], right.get(step, [])
            if who == "C" and not x and not y:      # (C is there for one step)
                continue
            notes = [r[1] for r in x + y if r[0] == "  (not compared)"]
            x = [r for r in x if r[0] != "  (not compared)"]
            y = [r for r in y if r[0] != "  (not compared)"]
            print(f"== {step}: {'same' if x == y else 'DIFFERENT'} ({len(x)} / {len(y)} rows)")
            different += x != y
            for i in range(max(len(x), len(y)) if x != y else 0):
                lhs = x[i] if i < len(x) else ("", "")
                rhs = y[i] if i < len(y) else ("", "")
                if lhs != rhs or verbose:
                    print(f"   {'  ' if lhs == rhs else '| '}{lhs[0]:16s}{lhs[1]}\n   {'  ' if lhs == rhs else '| '}{rhs[0]:16s}{rhs[1]}")
            if notes and verbose:
                print("   not compared: " + "; ".join(notes)[:300])
        if tree_ours and tree_ref:
            problems, extra = compare_trees(tree_ours, tree_ref)
            # Known, and in docs/architecture.md: vanilla completes /op and /deop itself.
            known = [p for p in problems if p in ("/op targets: suggestions is None, vanilla's 'minecraft:ask_server'",
                                                  "/deop targets: suggestions is None, vanilla's 'minecraft:ask_server'")]
            problems = [p for p in problems if p not in known]
            extra = extra + ["(known) " + p for p in known]
            print(f"== the command tree {who} has at the end: {'same, as far as ours goes' if not problems else 'DIFFERENT'} "
                  f"({len(tree_ours['children'])} commands of vanilla's {len(tree_ref['children'])}; "
                  f"{len(extra)} nodes of theirs are not in ours)")
            for p in problems:
                print("   | " + p)
            different += bool(problems)
            if verbose:
                for e in extra:
                    print("   not in ours: " + e)
    print(f"{different} steps differ")
    return different


def main() -> int:
    only = {word for arg in sys.argv if arg.startswith("--only=") for word in arg[len("--only="):].split(",") if word}
    sys.argv = [arg for arg in sys.argv if not arg.startswith("--only=")]
    if only and not any(word in name for name in SECTIONS for word in only):    # (a word may be another probe's)
        print(f"--only names no section; there are: {', '.join(SECTIONS)}")
        return 2
    if len(sys.argv) >= 4 and sys.argv[1] == "record":
        names = (sys.argv[4], sys.argv[5]) if len(sys.argv) >= 6 else ("ProbeE", "ProbeF")
        record(int(sys.argv[2]), sys.argv[3], *names, sys.argv[6] if len(sys.argv) >= 7 else None, only or None)
        return 0
    if len(sys.argv) >= 4 and sys.argv[1] == "diff":   # -v: every row, and what is not compared
        return 1 if diff(sys.argv[2], sys.argv[3]) else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
