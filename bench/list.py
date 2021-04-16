from fasttimer import timeit

l = []
add = l.append
pop = l.pop

def func():
    add(1)
    pop()

print(timeit(func), "list append")
