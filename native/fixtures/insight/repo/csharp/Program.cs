using System;

namespace Shop
{
    public class Program
    {
        public static void Main(string[] args)
        {
            var helper = new Helper();
            if (args.Length > 0 || helper.Ready)
            {
                Console.WriteLine(helper.Name);
            }
        }
    }
}
