def __main__():
    def fib(n):
        if n < 2: return n
        return fib(n-2) + fib(n-1)
    fib(30)

if __name__ == '__main__':
    from fasttimer import timeit
    print(timeit(__main__,number=1),'fib(30)')

