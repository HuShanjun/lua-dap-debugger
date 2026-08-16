-- No require("luadap"): runner preloads it.
local x, y = 10, 20
local sum = x + y
print("sum", sum)
print("DEBUGEE_DONE")
io.stdout:flush()
