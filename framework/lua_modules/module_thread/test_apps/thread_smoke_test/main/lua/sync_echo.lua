assert(args.text == "hello")
assert(args.count == 7)
assert(args.flag == true)
assert(args.items[1] == "one")
assert(args.items[2] == "two")
assert(args.nested.key == "value")

print("sync_echo ok text=" .. args.text .. " count=" .. tostring(args.count))
