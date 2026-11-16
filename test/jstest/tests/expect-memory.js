// expect: it ran out of room
var keep = [];
while (true) keep.push("x".repeat(1000) + keep.length);
