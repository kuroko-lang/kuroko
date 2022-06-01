import fasttimer as timeit

xs = [[[x for x in range(30)] for y in range(30)] for z in range(30)]
xs[29][29][29] = 42

def breaks():
  for ys in xs:
    for zs in ys:
      for z in zs:
        if z == 42:
          break
      else:
        continue
      break
    else:
      continue
    break
  else:
    return None
  return z

class FoundIt(Exception):
  def __init__(self, needle):
    self.needle = needle

def exceptions():
  try:
    for ys in xs:
      for zs in ys:
        for z in zs:
          if z == 42:
            raise FoundIt(z)
  except FoundIt as e:
    return e.needle

def _inner():
  for ys in xs:
    for zs in ys:
      for z in zs:
        if z == 42:
          return z

def functions():
  return _inner()

print(min(timeit.timeit(breaks,number=100) for x in range(10)),"breaks")
print(min(timeit.timeit(functions,number=100) for x in range(10)),"functions")
print(min(timeit.timeit(exceptions,number=100) for x in range(10)),"exceptions")
