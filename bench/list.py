from fasttimer import timeit

l = []
add = l.append
pop = l.pop

def func():
    add(1)
    pop()

print(min(timeit(func,number=100000) for x in range(10)), "list append")
