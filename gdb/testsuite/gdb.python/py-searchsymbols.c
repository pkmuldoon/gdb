typedef int fakeint;
int global_i;
fakeint global_fake_i;

void func1 (int ifunc1)
{
  int func_1_int=9;
  
  void func1_1 (int ifunc1_1)
  {
    int func_1_1_int=8;
    
    void func1_1_1 (int ifunc1_1_1)
    {
      int func_1_1_1_int=7;
      return; /* Break innermost. */
    }
    func1_1_1 (1);
  }
  func1_1 (2);
}

void func2 (int ifunc2)
{
  func1(3);
}

void main ()
{
  func2(4);
}
      
		    
