local pet = require("pet")

if not args or not args.id or args.id == "" then
    error("args.id is required")
end

pet.select(args.id)
print("Selected pet: " .. args.id)
