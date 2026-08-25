# Reproducible terminal demo: SWIM failure detection under a partition.
#
#   ./docs/demo-setup.sh && vhs docs/demo.tape
#
# Deterministic by construction (virtual clock, seeded SimNet drawn straight
# from mt19937_64), so re-recording on Linux produces the same numbers.

Output docs/demo.gif

Set Shell zsh
Set FontSize 14
Set Width 1180
Set Height 880
Set Padding 18
Set Theme "Builtin Dark"
Set TypingSpeed 20ms

Hide
Type "clear" Enter
Type "setopt interactive_comments" Enter
Type "alias mesh_node=./build/demo/mesh_node" Enter
Type "clear" Enter
Show

Sleep 600ms

Type "# Five nodes gossiping their membership. No coordinator, no heartbeat storm."
Enter
Sleep 1000ms
Type "# Partition one of them: how fast do the others notice, and do they overreact?"
Enter
Sleep 1400ms

Type "mesh_node"
Enter
Sleep 4000ms

Type "# 1020ms to the first SUSPECT, 2020ms to reconverge once the partition heals."
Enter
Sleep 1800ms
Type "# The last line is the one that matters: a reachable node was never confirmed dead."
Enter
Sleep 2000ms
Type "# Suspicion buys that. A node gets accused rather than executed, and can refute it."
Enter
Sleep 2600ms
