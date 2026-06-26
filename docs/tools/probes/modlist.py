help('modules')
print("==== dir(network) ====")
import network
print([a for a in dir(network) if not a.startswith('__')])
print("==== dir(Maix) ====")
import Maix
print([a for a in dir(Maix) if not a.startswith('__')])
