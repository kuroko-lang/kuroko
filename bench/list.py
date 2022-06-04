from fasttimer import timeit

if True:
    l = []

    def func():
        l.append(1)
        l.pop()

    print(min(timeit(func,number=100000) for x in range(10)), "list append")
