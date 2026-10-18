namespace Shop
{
    public abstract class Shape
    {
        public abstract double Area();
    }

    public class Helper
    {
        public bool Ready => true;
        public string Name { get; } = "helper";
    }
}
