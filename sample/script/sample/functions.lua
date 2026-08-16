functions = functions or {}

functions.add = function(a, b)
    print("functions.add", a, b)
    return a + b
end

functions.sub = function(a, b)
    return a - b
end

return functions