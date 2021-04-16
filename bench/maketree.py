class Node:
    def __init__(self, left, right):
        self.left = left
        self.right = right

def makeTree(depth):
    if depth <= 0: return Node(None,None)
    n1 = makeTree(depth-1)
    n2 = makeTree(depth-1)
    return Node(n1,n2)

if __name__ == '__main__':
    from fasttimer import timeit
    print(timeit(lambda: makeTree(16), number=10), 'makeTree')
